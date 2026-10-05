#include "common.h"

// In this opt-in mode a step is a sample of the current clock levels, not an
// implicit edge shared by all domains. The external harness supplies the time
// cadence and the blackbox clock models; no clock frequency is guessed here.
void graph::dynamicClockOptimize(std::map<std::string, Node*>& signals) {
  std::vector<Node*> consumers;
  for (const auto& entry : signals) {
    Node* node = entry.second;
    if (node->clock && (node->type == NODE_REG_DST || node->type == NODE_EXT ||
        node->type == NODE_WRITER || node->type == NODE_READWRITER ||
        node->type == NODE_SPECIAL)) consumers.push_back(node);
  }

  std::map<Node*, Node*> edges;
  auto edgeFor = [&](Node* clock) -> Node* {
    if (edges.count(clock)) return edges[clock];
    Assert(!clock->isArray(), "array clock %s is unsupported", clock->name.c_str());
    std::string base = "__gsim_clock_" + std::to_string(clock->id);
    auto makeNode = [&](NodeType type, const std::string& suffix) {
      Node* node = new Node(type);
      node->name = base + suffix;
      node->width = 1;
      node->sign = false;
      node->lineno = clock->lineno;
      node->reset = ZERO_RESET;
      Assert(!signals.count(node->name), "reserved clock-state name %s exists", node->name.c_str());
      signals[node->name] = node;
      return node;
    };
    // Simulator-owned history, updated once per sample irrespective of RTL
    // clocks. This must remain a register, so all users see the previous level.
    Node* previous = makeNode(NODE_REG_SRC, "$previous");
    Node* next = makeNode(NODE_REG_DST, "$previous$NEXT");
    previous->bindReg(next);
    previous->assignTree.push_back(new ExpTree(new ENode(next), previous));
    next->assignTree.push_back(new ExpTree(new ENode(clock), next));
    addReg(previous);

    Node* edge = makeNode(NODE_OTHERS, "$rising");
    ENode* notPrevious = new ENode(OP_NOT);
    notPrevious->addChild(new ENode(previous));
    ENode* rising = new ENode(OP_AND);
    rising->addChild(new ENode(clock));
    rising->addChild(notPrevious);
    edge->assignTree.push_back(new ExpTree(rising, edge));
    edges[clock] = edge;
    return edge;
  };

  for (Node* node : consumers) {
    Node* edge = edgeFor(node->clock);
    if (node->type == NODE_EXT) {
      node->clockTick = edge;
      for (Node* port : node->member) {
        if (port->type != NODE_EXT_OUT) continue;
        Assert(!port->isArray(), "clocked external array output %s is unsupported", port->name.c_str());
        Assert(!signals.count(port->extNextName()), "reserved external-state name %s exists", port->extNextName().c_str());
      }
      Assert(node->assignTree.size() == 1, "invalid external model %s", node->name.c_str());
      // Retain a real scheduling dependency on the clock edge, even when
      // registered external-model data dependencies are severed separately.
      node->assignTree[0]->getRoot()->addChild(new ENode(edge));
      continue;
    }
    if (node->type == NODE_REG_DST) {
      Node* source = node->getSrc();
      if (source->reset == UINTRESET) {
        // Synchronous reset belongs to this register's edge, not the global
        // resetAll() call at every sample. Last-connect priority preserves it.
        ENode* reset = new ENode(OP_WHEN);
        reset->addChild(source->resetCond->getRoot()->dup());
        reset->addChild(source->resetVal->getRoot()->dup());
        reset->addChild(nullptr);
        node->assignTree.push_back(new ExpTree(reset, node));
        source->resetTree = nullptr;
        source->reset = ZERO_RESET;
      }
    }
    for (ExpTree* tree : node->assignTree) {
      if (node->type == NODE_READWRITER && tree->isReadTree()) continue;
      ENode* guarded = new ENode(OP_WHEN);
      guarded->width = node->width;
      guarded->sign = node->sign;
      guarded->addChild(new ENode(edge));
      guarded->addChild(tree->getRoot());
      guarded->addChild(nullptr);
      tree->setRoot(guarded);
    }
  }
}
