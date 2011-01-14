// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "chrome/browser/ui/cocoa/cookie_tree_node.h"

#include "base/sys_string_conversions.h"

@implementation CocoaCookieTreeNode

- (id)initWithNode:(CookieTreeNode*)node {
  if ((self = [super init])) {
    DCHECK(node);
    treeNode_ = node;
    [self rebuild];
  }
  return self;
}

- (void)rebuild {
  title_.reset([base::SysUTF16ToNSString(treeNode_->GetTitle()) retain]);
  children_.reset();
  // The tree node assumes ownership of the cookie details object
  details_.reset([[CocoaCookieDetails createFromCookieTreeNode:(treeNode_)]
      retain]);
}

- (NSString*)title {
  return title_.get();
}

- (CocoaCookieDetailsType)nodeType {
  return [details_.get() type];
}

- (ui::TreeModelNode*)treeNode {
  return treeNode_;
}

- (NSMutableArray*)mutableChildren {
  if (!children_.get()) {
    const int childCount = treeNode_->GetChildCount();
    children_.reset([[NSMutableArray alloc] initWithCapacity:childCount]);
    for (int i = 0; i < childCount; ++i) {
      CookieTreeNode* child = treeNode_->GetChild(i);
      scoped_nsobject<CocoaCookieTreeNode> childNode(
          [[CocoaCookieTreeNode alloc] initWithNode:child]);
      [children_ addObject:childNode.get()];
    }
  }
  return children_.get();
}

- (NSArray*)children {
  return [self mutableChildren];
}

- (BOOL)isLeaf {
  return [self nodeType] != kCocoaCookieDetailsTypeFolder;
};

- (NSString*)description {
  NSString* format =
      @"<CocoaCookieTreeNode @ %p (title=%@, nodeType=%d, childCount=%u)";
  return [NSString stringWithFormat:format, self, [self title],
      [self nodeType], [[self children] count]];
}

- (CocoaCookieDetails*)details {
  return details_;
}

@end
