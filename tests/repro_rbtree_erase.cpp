// 最小复现：RbTree::doErase 的「双孩子」分支把树接坏
//
// 侵入式树的删除必须把**后继节点整棵挪进**被删位置（不能搬值——值就是对象本身，
// 搬了就没法摘链了）。修前的 co_async/utils/rbtree.hpp:177-206 在这条路上写错了三处：
//
//     :184  current->rbParent->rbLeft = replace->rbRight;   // 该写 replace->rbParent
//     :188  replace->rbParent = current;                    // 该指向 current->rbParent
//     :199  node = replace;                                 // 真正被摘掉的是 current
//
// 于是摘完后 replace 的 parent 指向一个已被摘除（甚至即将析构）的节点，
// 再拿它当「刚摘掉的节点」二次摘除：轻则丢整棵子树（front() 不再是全局最小），
// 重则指针成环或直接段错误。
//
// 触发条件只有一个：**析构一个当前有两个孩子的节点**。库里的定时器节点是
// `TimerNode : RbTree<TimerNode>::NodeType`，析构即 erase_from_parent -> doErase，
// 所以「被提前取消的定时器」正好走这条路——到期的那批走 front()（最小值，
// 没有左孩子，落在安全分支），取消的那批是任意节点，概率很高。
//
// 本用例用公开 API 覆盖：枚举 N 与「删哪个键」，每次删完都要求
// front() 仍是剩下键的最小值，且 front()+erase 走一遍出来的序列恰好是剩下的键升序。
//
// 修复前：N=4 删 1 之后 front() 还能对，但 N=20 左右开始丢子树/段错误。
// 修复后：全部组合退出 0。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_rbtree_erase -j4
//   ./build-dbg/tests/repro_rbtree_erase   # 退出 0
#include <co_async/std.hpp>
#include <co_async/utils/rbtree.hpp>

#include <vector>

using namespace co_async;

// 定时器节点的同构体：值就是节点，比较键是 v。
struct KeyNode : RbTree<KeyNode>::NodeType {
    int v;

    explicit KeyNode(int v) : v(v) {}

    friend bool operator<(KeyNode const &a, KeyNode const &b) {
        return a.v < b.v;
    }
};

static void require(bool ok, char const *what, int n, int victim) {
    if (!ok) {
        std::fprintf(stderr, "[repro_rbtree_erase] FAIL (n=%d, 删 %d): %s\n", n,
                     victim, what);
        std::abort();
    }
}

// 建 n 个键（升序插入，最坏形状），删掉 victim，然后逐项核对剩下的键。
static void runCase(int n, int victim) {
    RbTree<KeyNode> tree;
    std::vector<KeyNode *> nodes;
    for (int i = 1; i <= n; ++i) {
        nodes.push_back(new KeyNode(i));
        tree.insert(*nodes[i - 1]);
    }

    // 删除即析构：走 ~NodeType -> erase_from_parent -> doErase，
    // 与「取消一个定时器」是同一条路。
    delete nodes[victim - 1];
    nodes[victim - 1] = nullptr;

    std::vector<int> expect;
    for (int i = 1; i <= n; ++i) {
        if (i != victim) expect.push_back(i);
    }

    // 1) front() 必须还是全局最小
    if (!expect.empty()) {
        require(tree.front().v == expect.front(), "front() 不再是剩下的最小值",
                n, victim);
    }

    // 2) 逐个 front()+erase 走一遍，序列必须恰好是剩下的键升序
    std::vector<int> got;
    int guard = 0;
    while (!tree.empty()) {
        KeyNode &f = tree.front();
        got.push_back(f.v);
        tree.erase(f);
        if (++guard > n + 5) {
            require(false, "erase 走不动了（树成环）", n, victim);
        }
    }
    require(got == expect, "剩余键序列与期望不一致", n, victim);

    for (auto *p : nodes) {
        delete p;
    }
}

int main() {
    std::setlocale(LC_ALL, "");
    for (int n = 1; n <= 200; ++n) {
        for (int victim = 1; victim <= n; ++victim) {
            runCase(n, victim);
        }
    }
    std::fprintf(stderr, "[repro_rbtree_erase] PASS (n=1..200 全组合)\n");
    return 0;
}
