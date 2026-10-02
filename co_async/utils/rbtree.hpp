#pragma once
#include <co_async/std.hpp>

namespace co_async {
template <class Value, class Compare = std::less<>>
struct RbTree : private Compare {
private:
    enum RbColor {
        RED,
        BLACK
    };

protected:
    struct RbNode {
        RbNode() noexcept
            : rbLeft(nullptr),
              rbRight(nullptr),
              rbParent(nullptr),
              rbTree(nullptr),
              rbColor(RED) {}
        friend struct RbTree;

    private:
        RbNode *rbLeft;
        RbNode *rbRight;
        RbNode *rbParent;

    protected:
        RbTree *rbTree;

    private:
        RbColor rbColor;
    };

public:
    struct NodeType : RbNode {
        NodeType() = default;
        NodeType(NodeType &&) = delete;

        ~NodeType() noexcept {
            erase_from_parent();
        }

    protected:
        void erase_from_parent() {
            static_assert(
                std::is_base_of_v<NodeType, Value>,
                "Value type must be derived from RbTree<Value>::NodeType");
            if (this->rbTree) {
                this->rbTree->doErase(this);
                this->rbTree = nullptr;
            }
        }
    };

private:
    RbNode *root;

    bool compare(RbNode *left, RbNode *right) const noexcept {
        return static_cast<Compare const &>(*this)(
            static_cast<Value &>(*left), static_cast<Value &>(*right));
    }

    void rotateLeft(RbNode *node) noexcept {
        RbNode *rightChild = node->rbRight;
        node->rbRight = rightChild->rbLeft;
        if (rightChild->rbLeft != nullptr) {
            rightChild->rbLeft->rbParent = node;
        }
        rightChild->rbParent = node->rbParent;
        if (node->rbParent == nullptr) {
            root = rightChild;
        } else if (node == node->rbParent->rbLeft) {
            node->rbParent->rbLeft = rightChild;
        } else {
            node->rbParent->rbRight = rightChild;
        }
        rightChild->rbLeft = node;
        node->rbParent = rightChild;
    }

    void rotateRight(RbNode *node) noexcept {
        RbNode *leftChild = node->rbLeft;
        node->rbLeft = leftChild->rbRight;
        if (leftChild->rbRight != nullptr) {
            leftChild->rbRight->rbParent = node;
        }
        leftChild->rbParent = node->rbParent;
        if (node->rbParent == nullptr) {
            root = leftChild;
        } else if (node == node->rbParent->rbRight) {
            node->rbParent->rbRight = leftChild;
        } else {
            node->rbParent->rbLeft = leftChild;
        }
        leftChild->rbRight = node;
        node->rbParent = leftChild;
    }

    void fixViolation(RbNode *node) noexcept {
        RbNode *parent = nullptr;
        RbNode *grandParent = nullptr;
        while (node != root && node->rbColor != BLACK &&
               node->rbParent->rbColor == RED) {
            parent = node->rbParent;
            grandParent = parent->rbParent;
            if (parent == grandParent->rbLeft) {
                RbNode *uncle = grandParent->rbRight;
                if (uncle != nullptr && uncle->rbColor == RED) {
                    grandParent->rbColor = RED;
                    parent->rbColor = BLACK;
                    uncle->rbColor = BLACK;
                    node = grandParent;
                } else {
                    if (node == parent->rbRight) {
                        rotateLeft(parent);
                        node = parent;
                        parent = node->rbParent;
                    }
                    rotateRight(grandParent);
                    std::swap(parent->rbColor, grandParent->rbColor);
                    node = parent;
                }
            } else {
                RbNode *uncle = grandParent->rbLeft;
                if (uncle != nullptr && uncle->rbColor == RED) {
                    grandParent->rbColor = RED;
                    parent->rbColor = BLACK;
                    uncle->rbColor = BLACK;
                    node = grandParent;
                } else {
                    if (node == parent->rbLeft) {
                        rotateRight(parent);
                        node = parent;
                        parent = node->rbParent;
                    }
                    rotateLeft(grandParent);
                    std::swap(parent->rbColor, grandParent->rbColor);
                    node = parent;
                }
            }
        }
        root->rbColor = BLACK;
    }

    void doInsert(RbNode *node) noexcept {
        node->rbLeft = nullptr;
        node->rbRight = nullptr;
        node->rbTree = this;
        node->rbColor = RED;
        RbNode *parent = nullptr;
        RbNode *current = root;
        while (current != nullptr) {
            parent = current;
            if (compare(node, current)) {
                current = current->rbLeft;
            } else {
                current = current->rbRight;
            }
        }
        node->rbParent = parent;
        if (parent == nullptr) {
            root = node;
        } else if (compare(node, parent)) {
            parent->rbLeft = node;
        } else {
            parent->rbRight = node;
        }
        fixViolation(node);
    }

    // 用 v 顶替 u 在树里的位置。u 的位置由调用方负责不再被引用。
    void moveInto(RbNode *u, RbNode *v) noexcept {
        if (u->rbParent == nullptr) {
            root = v;
        } else if (u == u->rbParent->rbLeft) {
            u->rbParent->rbLeft = v;
        } else {
            u->rbParent->rbRight = v;
        }
        if (v != nullptr) {
            v->rbParent = u->rbParent;
        }
    }

    // 删除后的双黑修复。node 是丢了黑色的那个位置，可能为空；此时 gapParent
    // 给出这个空位置挂在谁下面。每轮用 node 自己的 rbParent 覆盖它。
    void fixLostBlack(RbNode *node, RbNode *gapParent) noexcept {
        while (node != root && (node == nullptr || node->rbColor == BLACK)) {
            if (node != nullptr) {
                gapParent = node->rbParent;
            }
            if (gapParent == nullptr) {
                break;
            }
            if (node == gapParent->rbLeft) {
                RbNode *brother = gapParent->rbRight;
                if (brother == nullptr) {
                    break;
                }
                if (brother->rbColor == RED) {
                    brother->rbColor = BLACK;
                    gapParent->rbColor = RED;
                    rotateLeft(gapParent);
                    brother = gapParent->rbRight;
                    if (brother == nullptr) {
                        break;
                    }
                }
                bool farBlack = brother->rbRight == nullptr ||
                                  brother->rbRight->rbColor == BLACK;
                bool nearBlack = brother->rbLeft == nullptr ||
                                  brother->rbLeft->rbColor == BLACK;
                if (farBlack && nearBlack) {
                    brother->rbColor = RED;
                    node = gapParent;
                } else {
                    if (farBlack) {
                        brother->rbLeft->rbColor = BLACK;
                        brother->rbColor = RED;
                        rotateRight(brother);
                        brother = gapParent->rbRight;
                        if (brother == nullptr) {
                            break;
                        }
                    }
                    brother->rbColor = gapParent->rbColor;
                    gapParent->rbColor = BLACK;
                    brother->rbRight->rbColor = BLACK;
                    rotateLeft(gapParent);
                    node = root;
                }
            } else {
                RbNode *brother = gapParent->rbLeft;
                if (brother == nullptr) {
                    break;
                }
                if (brother->rbColor == RED) {
                    brother->rbColor = BLACK;
                    gapParent->rbColor = RED;
                    rotateRight(gapParent);
                    brother = gapParent->rbLeft;
                    if (brother == nullptr) {
                        break;
                    }
                }
                bool farBlack = brother->rbLeft == nullptr ||
                                  brother->rbLeft->rbColor == BLACK;
                bool nearBlack = brother->rbRight == nullptr ||
                                  brother->rbRight->rbColor == BLACK;
                if (farBlack && nearBlack) {
                    brother->rbColor = RED;
                    node = gapParent;
                } else {
                    if (farBlack) {
                        brother->rbRight->rbColor = BLACK;
                        brother->rbColor = RED;
                        rotateLeft(brother);
                        brother = gapParent->rbLeft;
                        if (brother == nullptr) {
                            break;
                        }
                    }
                    brother->rbColor = gapParent->rbColor;
                    gapParent->rbColor = BLACK;
                    brother->rbLeft->rbColor = BLACK;
                    rotateRight(gapParent);
                    node = root;
                }
            }
        }
        if (node != nullptr) {
            node->rbColor = BLACK;
        }
    }

    void doErase(RbNode *current) noexcept {
        current->rbTree = nullptr;

        RbNode *child = nullptr;         // 顶上它位置的子树（可能为空）
        RbNode *gapParent = nullptr; // child 为空时，空位置挂谁下面
        RbColor color = RED;

        if (current->rbLeft != nullptr && current->rbRight != nullptr) {
            // 侵入式容器不能搬值，只能把后继节点整棵挪进被删位置。
            RbNode *replace = current->rbRight;
            while (replace->rbLeft != nullptr) {
                replace = replace->rbLeft;
            }
            color = replace->rbColor;
            child = replace->rbRight;
            gapParent = replace->rbParent;

            if (replace->rbParent != current) {
                moveInto(replace, replace->rbRight);
                replace->rbRight = current->rbRight;
                replace->rbRight->rbParent = replace;
            } else {
                gapParent = replace;
            }
            moveInto(current, replace);
            replace->rbLeft = current->rbLeft;
            replace->rbLeft->rbParent = replace;
            replace->rbColor = current->rbColor;
        } else {
            color = current->rbColor;
            child = (current->rbLeft != nullptr) ? current->rbLeft
                                                 : current->rbRight;
            gapParent = current->rbParent;
            moveInto(current, child);
        }
        if (child != nullptr) {
            gapParent = child->rbParent;
        }

        if (color == BLACK) {
            fixLostBlack(child, gapParent);
        }
    }

    RbNode *getFront() const noexcept {
        RbNode *current = root;
        while (current->rbLeft != nullptr) {
            current = current->rbLeft;
        }
        return current;
    }

    RbNode *getBack() const noexcept {
        RbNode *current = root;
        while (current->rbRight != nullptr) {
            current = current->rbRight;
        }
        return current;
    }

    template <class Visitor>
    void doTraverseInorder(RbNode *node, Visitor &&visitor) {
        if (node == nullptr) {
            return;
        }
        doTraverseInorder(node->rbLeft, visitor);
        visitor(node);
        doTraverseInorder(node->rbRight, visitor);
    }

    void doClear(RbNode *node) {
        if (node == nullptr) {
            return;
        }
        doClear(node->rbLeft);
        node->rbTree = nullptr;
        doClear(node->rbRight);
    }

    void doClear() {
        doClear(root);
        root = nullptr;
    }

public:
    RbTree() noexcept(noexcept(Compare())) : Compare(), root(nullptr) {}

    explicit RbTree(Compare comp) noexcept(noexcept(Compare(comp)))
        : Compare(comp),
          root(nullptr) {}

    RbTree(RbTree &&) = delete;

    ~RbTree() noexcept {}

    void insert(Value &value) noexcept {
        doInsert(&static_cast<RbNode &>(value));
    }

    void erase(Value &value) noexcept {
        doErase(&static_cast<RbNode &>(value));
    }

    bool empty() const noexcept {
        return root == nullptr;
    }

    Value &front() const noexcept {
        return static_cast<Value &>(*getFront());
    }

    Value &back() const noexcept {
        return static_cast<Value &>(*getBack());
    }

    template <class Visitor, class V>
    std::pair<RbNode *, RbNode *> traverseEqualRange(Visitor &&visitor,
                                                     V &&value) {}

    template <class Visitor>
    void traverseInorder(Visitor &&visitor) {
        doTraverseInorder(root, [visitor = std::forward<Visitor>(visitor)](
                                    RbNode *node) mutable {
            visitor(static_cast<Value &>(*node));
        });
    }

    void clear() {
        doClear();
    }
};

// template <class Value, class Compare = std::less<>>
// struct ConcurrentRbTree : private RbTree<Value, Compare> {
// private:
//     using BaseTree = RbTree<Value, Compare>;
//
// public:
//     struct NodeType : BaseTree::RbNode {
//         NodeType() = default;
//         NodeType(NodeType &&) = delete;
//
//         ~NodeType() noexcept {
//             erase_from_parent();
//         }
//
//     protected:
//         void erase_from_parent() {
//             static_assert(
//                 std::is_base_of_v<NodeType, Value>,
//                 "Value type must be derived from RbTree<Value>::NodeType");
//             if (this->rbTree) {
//                 auto lock = static_cast<ConcurrentRbTree
//                 *>(this->rbTree)->lock(); lock->erase(static_cast<Value
//                 &>(*this)); this->rbTree = nullptr;
//             }
//         }
//     };
//
//     struct LockGuard {
//     private:
//         BaseTree *mThat;
//         std::unique_lock<std::mutex> mGuard;
//
//         explicit LockGuard(ConcurrentRbTree *that) noexcept
//             : mThat(that),
//               mGuard(that->mMutex) {}
//
//         friend ConcurrentRbTree;
//
//     public:
//         BaseTree &operator*() const noexcept {
//             return *mThat;
//         }
//
//         BaseTree *operator->() const noexcept {
//             return mThat;
//         }
//
//         void unlock() noexcept {
//             mGuard.unlock();
//             mThat = nullptr;
//         }
//     };
//
//     LockGuard lock() noexcept {
//         return LockGuard(this);
//     }
//
// private:
//     std::mutex mMutex;
// };
} // namespace co_async
