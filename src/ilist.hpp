template <class Node>
struct ilist {
  Node *head;
  Node *tail;

  ilist() { head = tail = nullptr; }

  // insert newNode before insertBefore
  void insertBefore(Node *newNode, Node *insertBefore) {
    newNode->prev = insertBefore->prev;
    newNode->next = insertBefore;
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

    node->prev = node->next = nullptr;
  }
};
