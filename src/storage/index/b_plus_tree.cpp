#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *txn) -> bool {
  Context ctx;

  auto header_page_guard = bpm_->FetchPageRead(header_page_id_);
  auto header_page = header_page_guard.template As<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID) return false;
  ctx.root_page_id_ = header_page->root_page_id_;
  ctx.read_set_.push_back(bpm_->FetchPageRead(ctx.root_page_id_));

  FindLeafPage(key, Operation::Search, ctx);
  auto leaf_page = ctx.read_set_.back().template As<LeafPage>();
  int index = BinaryFind(leaf_page, key);
  if (index == -1 || comparator_(leaf_page->KeyAt(index), key) != 0) return false;
  result->push_back(leaf_page->ValueAt(index));
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::FindLeafPage(const KeyType &key, Operation op, Context &ctx) {
  if (op == Operation::Search) {
    auto page = ctx.read_set_.back().template As<BPlusTreePage>();
    while (!page->IsLeafPage()) {
      auto internal = ctx.read_set_.back().template As<InternalPage>();
      auto next_page_id = internal->ValueAt(BinaryFind(internal, key));
      ctx.read_set_.push_back(bpm_->FetchPageRead(next_page_id));
      // ctx.read_set_.pop_front();
      page = ctx.read_set_.back().template As<BPlusTreePage>();
    }
    return;
  }
  if (op == Operation::Insert || op == Operation::Remove) {
    auto page = ctx.write_set_.back().As<BPlusTreePage>();
    while (!page->IsLeafPage()) {
      auto internal = ctx.write_set_.back().As<InternalPage>();
      auto next_page_id = internal->ValueAt(BinaryFind(internal, key));
      ctx.write_set_.push_back(bpm_->FetchPageWrite(next_page_id));
      if (IsSafePage(ctx.write_set_.back().template As<BPlusTreePage>(), op, false)) {
        while (ctx.write_set_.size() > 1) {
          ctx.write_set_.pop_front();
        }
      } // child is safe
      page = ctx.write_set_.back().template As<BPlusTreePage>();
    }
    return;
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsSafePage(const BPlusTreePage *tree_page, Operation op, bool isRootPage) -> bool {
  if (op == Operation::Search) {
    return true;
  }
  if (op == Operation::Insert) {
    if (tree_page->IsLeafPage()) {
      return tree_page->GetSize() + 1 < tree_page->GetMaxSize();
    }
    return tree_page->GetSize() < tree_page->GetMaxSize();
  }
  if (op == Operation::Remove) { // 删了之后仍安全
    if (isRootPage) {
      if (tree_page->IsLeafPage()) {
        return tree_page->GetSize() > 1;
      }
      return tree_page->GetSize() > 2;
    }
    return tree_page->GetSize() > tree_page->GetMinSize();
  }
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *txn)
    -> bool {
  Context ctx;
  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  auto header_page = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();

  if (header_page->root_page_id_ == INVALID_PAGE_ID) {  // root not exist, start a new tree
    auto root_guard = bpm_->NewPageGuarded(&ctx.root_page_id_);
    header_page->root_page_id_ = ctx.root_page_id_;
    auto leaf_page = root_guard.AsMut<LeafPage>();

    leaf_page->Init(leaf_max_size_);
    leaf_page->SetSize(1);

    leaf_page->SetAt(0, key, value);
    ctx.Drop();
    return true;
  }
  ctx.root_page_id_ = header_page->root_page_id_;
  ctx.write_set_.push_back(bpm_->FetchPageWrite(ctx.root_page_id_));
  if (IsSafePage(ctx.write_set_.back().As<BPlusTreePage>(), Operation::Insert, true)) {
    ctx.header_page_ = std::nullopt;  // unlock header_page
  }
  FindLeafPage(key, Operation::Insert, ctx);
  auto &leaf_page_guard = ctx.write_set_.back();
  auto leaf_page = leaf_page_guard.AsMut<LeafPage>();

  int index = BinaryFind(leaf_page, key);
  if (index != -1 && comparator_(leaf_page->KeyAt(index), key) == 0) {  // key exists
    ctx.Drop();
    return false;
  }

  ++index;
  leaf_page->IncreaseSize(1);
  for (int i = leaf_page->GetSize(); i > index; --i) {
    leaf_page->SetAt(i, leaf_page->KeyAt(i - 1), leaf_page->ValueAt(i - 1));
  }
  leaf_page->SetAt(index, key, value);

  if (leaf_page->GetSize() < leaf_page->GetMaxSize()) {  // 叶子节点未溢出，不需要分裂
    ctx.Drop();
    return true;
  }

  // == max size, split
  auto new_page_id = 0;
  auto new_leaf_page_guard = bpm_->NewPageGuarded(&new_page_id);
  auto new_leaf_page = new_leaf_page_guard.template AsMut<LeafPage>();

  leaf_page->SetNextPageId(new_leaf_page_guard.PageId());
  new_leaf_page->Init(leaf_max_size_);
  new_leaf_page->SetSize(leaf_page->GetSize() - leaf_page->GetMinSize());
  new_leaf_page->SetNextPageId(leaf_page->GetNextPageId());

  for(int i = leaf_page->GetMinSize(); i < leaf_page->GetSize(); ++i) {
    new_leaf_page->SetAt(i-leaf_page->GetMinSize(), leaf_page->KeyAt(i), leaf_page->ValueAt(i));
  }
  leaf_page->SetSize(leaf_page->GetMinSize());

  KeyType split_key = new_leaf_page->KeyAt(0);
  // 将split_key插入父节点
  InsertIntoParent(split_key, new_leaf_page_guard.PageId(), ctx, ctx.write_set_.size() - 2); // parent page
  ctx.Drop(); // 释放锁
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(const KeyType &key, page_id_t new_child_id, Context &ctx, int index) {
  if (index < 0) {  // parent is header_page
    auto new_root_page_id = 0;
    auto new_root_page_guard = bpm_->NewPageGuarded(&new_root_page_id);
    auto new_root_page = new_root_page_guard.template AsMut<InternalPage>();

    new_root_page->Init(internal_max_size_);
    new_root_page->SetSize(2);

    new_root_page->SetValueAt(0, ctx.write_set_[index + 1].PageId());
    new_root_page->SetKeyAt(1, key);
    new_root_page->SetValueAt(1, new_child_id);
    auto header_page = ctx.header_page_->template AsMut<BPlusTreeHeaderPage>();
    header_page->root_page_id_ = new_root_page_id;
    return;
  }
  auto parent_page = ctx.write_set_[index].AsMut<InternalPage>();

  if (parent_page->GetSize() != parent_page->GetMaxSize()) {  // 父节点不需要分裂
    int index = BinaryFind(parent_page, key) + 1;
    parent_page->IncreaseSize(1);
    for (int i = parent_page->GetSize()-1; i > index; --i) {
      parent_page->SetKeyAt(i, parent_page->KeyAt(i - 1));
      parent_page->SetValueAt(i, parent_page->ValueAt(i - 1));
    }
    parent_page->SetKeyAt(index, key);
    parent_page->SetValueAt(index, new_child_id);
    return;
  }

  // > max size, split
  auto new_parent_page_id = 0;
  auto new_parent_page_guard = bpm_->NewPageGuarded(&new_parent_page_id);
  auto new_parent_page = new_parent_page_guard.AsMut<InternalPage>();
  
  new_parent_page->Init(internal_max_size_);
  new_parent_page->SetSize(parent_page->GetMaxSize() + 1 - parent_page->GetMinSize());

  int pos = BinaryFind(parent_page, key) + 1;
  if (pos < parent_page->GetMinSize()) {  // key插入到左边
    for (int i = parent_page->GetMinSize(); i < parent_page->GetSize(); ++i) {
      new_parent_page->SetKeyAt(i - parent_page->GetMinSize() + 1, parent_page->KeyAt(i));
      new_parent_page->SetValueAt(i - parent_page->GetMinSize() + 1, parent_page->ValueAt(i));
    }
    new_parent_page->SetKeyAt(0, parent_page->KeyAt(parent_page->GetMinSize()-1));
    new_parent_page->SetValueAt(0, parent_page->ValueAt(parent_page->GetMinSize()-1));
    for(int i = parent_page->GetMinSize()-1; i>pos; --i) {
      parent_page->SetKeyAt(i, parent_page->KeyAt(i-1));
      parent_page->SetValueAt(i, parent_page->ValueAt(i-1));
    }
    parent_page->SetKeyAt(pos, key);
    parent_page->SetValueAt(pos, new_child_id);
  } else if (pos == parent_page->GetMinSize()) {  // key插入到中间
    for (int i = parent_page->GetMinSize(); i < parent_page->GetSize(); ++i) {
      new_parent_page->SetKeyAt(i - parent_page->GetMinSize() + 1, parent_page->KeyAt(i));
      new_parent_page->SetValueAt(i - parent_page->GetMinSize() + 1, parent_page->ValueAt(i));
    }
    new_parent_page->SetValueAt(0, new_child_id);
    new_parent_page->SetKeyAt(0, key);
  } else {  // key插入到右边
    for (int i = parent_page->GetMinSize(); i < parent_page->GetSize(); ++i) {
      new_parent_page->SetKeyAt(i - parent_page->GetMinSize(), parent_page->KeyAt(i));
      new_parent_page->SetValueAt(i - parent_page->GetMinSize(), parent_page->ValueAt(i));
    }
    pos -= parent_page->GetMinSize();
    for (int i = new_parent_page->GetSize()-1; i > pos; --i) {
      new_parent_page->SetKeyAt(i, new_parent_page->KeyAt(i-1));
      new_parent_page->SetValueAt(i, new_parent_page->ValueAt(i-1));
    }
    new_parent_page->SetKeyAt(pos, key);
    new_parent_page->SetValueAt(pos, new_child_id);
  }

  parent_page->SetSize(parent_page->GetMinSize());
  InsertIntoParent(new_parent_page->KeyAt(0), new_parent_page_id, ctx, index - 1);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *txn) {
  // Declaration of context instance.
  Context ctx;
  ctx.header_page_ = bpm_->FetchPageWrite(header_page_id_);
  auto header_page = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID) return;

  ctx.root_page_id_ = header_page->root_page_id_;
  ctx.write_set_.push_back(bpm_->FetchPageWrite(ctx.root_page_id_));
  if (IsSafePage(ctx.write_set_.back().As<BPlusTreePage>(), Operation::Remove, true)) {
    ctx.header_page_ = std::nullopt;  // unlock header_page
  }
  FindLeafPage(key, Operation::Remove, ctx);
  auto &leaf_page_guard = ctx.write_set_.back();
  auto leaf_page = leaf_page_guard.AsMut<LeafPage>();
  int pos = BinaryFind(leaf_page, key);
  if (pos == -1 || comparator_(leaf_page->KeyAt(pos), key) != 0) {
    ctx.Drop();
    return;
  }
  // key exists
  for (int i = pos + 1; i < leaf_page->GetSize(); ++i) {
    leaf_page->SetAt(i - 1, leaf_page->KeyAt(i), leaf_page->ValueAt(i));
  }
  leaf_page->SetSize(leaf_page->GetSize() - 1);

  if (leaf_page->GetSize() >= leaf_page->GetMinSize()) {
    ctx.Drop();
    return;
  }

  // underflow
  if (ctx.IsRootPage(leaf_page_guard.PageId())) {  // 该叶子节点是根节点
    if (leaf_page->GetSize() == 0) {               // size为0
      header_page->root_page_id_ = INVALID_PAGE_ID;
    }
    ctx.Drop();
    return;
  }

  auto &parent_page_guard = ctx.write_set_[ctx.write_set_.size() - 2];
  auto parent_page = parent_page_guard.AsMut<InternalPage>();
  auto index = BinaryFind(parent_page, key);

  // 如果有右brother
  if (index < parent_page->GetSize() - 1) {
    page_id_t right_brother_page_id = parent_page->ValueAt(index + 1);
    auto right_brother_page_guard = bpm_->FetchPageWrite(right_brother_page_id);
    auto right_brother_page = right_brother_page_guard.AsMut<LeafPage>();

    auto merge_size = right_brother_page->GetSize() + leaf_page->GetSize();
    if (merge_size < leaf_page->GetMaxSize()) {  // 可以合并
      // merge
      int s = leaf_page->GetSize();
      leaf_page->SetSize(merge_size);
      for (int i = 0; i < right_brother_page->GetSize(); ++i) {
        leaf_page->SetAt(i + s, right_brother_page->KeyAt(i), right_brother_page->ValueAt(i));
      }
      leaf_page->SetNextPageId(right_brother_page->GetNextPageId());
      
      RemoveFromParent(index + 1, ctx, ctx.write_set_.size() - 2);
    } else {
      // borrow
      leaf_page->IncreaseSize(1);
      leaf_page->SetAt(leaf_page->GetSize()-1, right_brother_page->KeyAt(0), right_brother_page->ValueAt(0));
      for (int i = 0; i < right_brother_page->GetSize() - 1; ++i) {
        right_brother_page->SetAt(i, right_brother_page->KeyAt(i+1), right_brother_page->ValueAt(i+1));
      }
      right_brother_page->SetSize(right_brother_page->GetSize() - 1);
      parent_page->SetKeyAt(index + 1, right_brother_page->KeyAt(0));
    }
  } else {
    // 左brother
    page_id_t left_brother_page_id = parent_page->ValueAt(index - 1);
    auto left_brother_page_guard = bpm_->FetchPageWrite(left_brother_page_id);
    auto left_brother_page = left_brother_page_guard.AsMut<LeafPage>();

    auto merge_size = left_brother_page->GetSize() + leaf_page->GetSize();
    if (merge_size < left_brother_page->GetMaxSize()) {  // 可以合并
      // merge
      int s = left_brother_page->GetSize();
      left_brother_page->SetSize(merge_size);
      for (int i = 0; i < leaf_page->GetSize(); ++i) {
        left_brother_page->SetAt(i + s, leaf_page->KeyAt(i), leaf_page->ValueAt(i));
      }
      left_brother_page->SetNextPageId(leaf_page->GetNextPageId());
      RemoveFromParent(index, ctx, ctx.write_set_.size() - 2);
    } else {
      // borrow
      leaf_page->IncreaseSize(1);
      for (int i = leaf_page->GetSize() - 1; i >= 1; --i) {
        leaf_page->SetAt(i, leaf_page->KeyAt(i-1), leaf_page->ValueAt(i-1));
      }
      leaf_page->SetAt(0, left_brother_page->KeyAt(left_brother_page->GetSize() - 1), left_brother_page->ValueAt(left_brother_page->GetSize() - 1));
      left_brother_page->SetSize(left_brother_page->GetSize() - 1);
      parent_page->SetKeyAt(index, leaf_page->KeyAt(0));
    }
  }
  ctx.Drop();
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromParent(int valueIndex, Context &ctx, int index) {
  auto &page_guard = ctx.write_set_[index];
  auto page = page_guard.AsMut<InternalPage>();
  for (int i = valueIndex + 1; i < page->GetSize(); ++i) {
    page->SetKeyAt(i - 1, page->KeyAt(i));
    page->SetValueAt(i - 1, page->ValueAt(i));
  }
  page->SetSize(page->GetSize() - 1);

  if (page->GetSize() >= page->GetMinSize()) {  // 无underflow
    return;
  }
  // underflow
  if (ctx.IsRootPage(page_guard.PageId())) {  // 该page是根节点
    if (page->GetSize() == 1) {               // 根节点需要更换了
      auto header_page = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = page->ValueAt(0);
    }
    return;
  }
  auto &parent_page_guard = ctx.write_set_[index - 1];
  auto parent_page = parent_page_guard.AsMut<InternalPage>();
  auto pos = parent_page->ValueIndex(page_guard.PageId()); //
  // 如果有右brother
  if (pos < parent_page->GetSize() - 1) {
    page_id_t right_brother_page_id = parent_page->ValueAt(pos + 1);
    auto right_brother_page_guard = bpm_->FetchPageWrite(right_brother_page_id);
    auto right_brother_page = right_brother_page_guard.AsMut<InternalPage>();

    auto merge_size = right_brother_page->GetSize() + page->GetSize() ;
    if (merge_size <= page->GetMaxSize()) {
      // merge
      int s = page->GetSize();
      page->SetSize(merge_size);
      for (int i = 0; i < right_brother_page->GetSize(); ++i) {
        page->SetKeyAt(i + s, right_brother_page->KeyAt(i));
        page->SetValueAt(i + s, right_brother_page->ValueAt(i));
      }
      RemoveFromParent(pos + 1, ctx, index - 1);
    } else {
      // borrow
      page->IncreaseSize(1);
      page->SetKeyAt(page->GetSize()-1, right_brother_page->KeyAt(0));
      page->SetValueAt(page->GetSize()-1, right_brother_page->ValueAt(0));
      for (int i = 0; i < right_brother_page->GetSize() - 1; ++i) {
        right_brother_page->SetKeyAt(i, right_brother_page->KeyAt(i+1));
        right_brother_page->SetValueAt(i, right_brother_page->ValueAt(i+1));
      }
      right_brother_page->SetSize(right_brother_page->GetSize() - 1);
      parent_page->SetKeyAt(pos + 1, right_brother_page->KeyAt(0));
    }
  } else {
    // 左brother
    page_id_t left_brother_page_id = parent_page->ValueAt(pos - 1);
    auto left_brother_page_guard = bpm_->FetchPageWrite(left_brother_page_id);
    auto left_brother_page = left_brother_page_guard.AsMut<InternalPage>();

    auto merge_size = left_brother_page->GetSize() + page->GetSize();
    if (merge_size <= left_brother_page->GetMaxSize()) {  // 可以合并
      // merge
      int s = left_brother_page->GetSize();
      left_brother_page->SetSize(merge_size);
      for (int i = 0; i < page->GetSize(); ++i) {
        left_brother_page->SetKeyAt(i + s, page->KeyAt(i));
        left_brother_page->SetValueAt(i + s, page->ValueAt(i));
      }
      RemoveFromParent(pos, ctx, index - 1);
    } else {
      // borrow
      page->IncreaseSize(1);
      for (int i = page->GetSize() -1; i >= 1; --i) {
        page->SetKeyAt(i, page->KeyAt(i - 1));
        page->SetValueAt(i, page->ValueAt(i - 1));
      }
      page->SetKeyAt(0, left_brother_page->KeyAt(left_brother_page->GetSize() - 1));
      page->SetValueAt(0, left_brother_page->ValueAt(left_brother_page->GetSize() - 1));   
      left_brother_page->SetSize(left_brother_page->GetSize() - 1);
      parent_page->SetKeyAt(pos, page->KeyAt(0));
    }
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/

// lower_bound
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
     ->  int
{
  int l = 0;
  int r = leaf_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(leaf_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r >= 0 && comparator_(leaf_page -> KeyAt(r), key) == 1)
  {
    r = -1;
  }

  return r;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key)  ->  int
{
  int l = 1;
  int r = internal_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(internal_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r == -1 || comparator_(internal_page -> KeyAt(r), key) == 1)
  {
    r = 0;
  }

  return r;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = root_header_page -> root_page_id_;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub