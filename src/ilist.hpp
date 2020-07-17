template <class node>
struct ilist {
  node *head;
  node *tail;

  ilist() { head = tail = nullptr; }

  // insert newNode before insertBefore
  void insertBefore(node *newNode, node *insertBefore) { 
      newNode->prev = insertBefore->prev;
      newNode->next = insertBefore;
      insertBefore->prev = newNode;

      if (head == insertBefore) {
        head = newNode;
      }
  }

  // insert newNode at the end of ilist
  void insertAtEnd(node *newNode) { 
      newNode->prev = tail;
      newNode->next = nullptr;

      if (tail == nullptr) {
        head = tail = newNode;
      } else {
        tail->next = newNode;
        tail = newNode;
      }
  }
};