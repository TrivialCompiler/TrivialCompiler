#pragma once

template <class Node>
struct ilist {
  Node *head;
  Node *tail;

  ilist() { head = tail = nullptr; }

  // insert newNode before insertBefore
  void insertBefore(Node *newNode, Node *insertBefore) {
    newNode->prev = insertBefore->prev;
    newNode->next = insertBefore;
    if (insertBefore->prev) {
      insertBefore->prev->next = newNode;
    }
    insertBefore->prev = newNode;

    if (head == insertBefore) {
      head = newNode;
    }
  }

  // insert newNode at the end of ilist
  void insertAtEnd(Node *newNode) {
    newNode->prev = tail;
    newNode->next = nullptr;

    if (tail == nullptr) {
      head = tail = newNode;
    } else {
      tail->next = newNode;
      tail = newNode;
    }
  }

  // insert newNode at the begin of ilist
  void insertAtBegin(Node *newNode) {
    newNode->prev = nullptr;
    newNode->next = head;

    if (head == nullptr) {
      head = tail = newNode;
    } else {
      head->prev = newNode;
      head = newNode;
    }
  }

  // remove node from ilist
  void remove(Node *node) {
    if (node->prev != nullptr) {
      node->prev->next = node->next;
    } else {
      head = node->next;
    }

    if (node->next != nullptr) {
      node->next->prev = node->prev;
    } else {
      tail = node->prev;
    }
  }
};
