#include "common.h"
#include <map>
#include <stack>

class clockVal {
public:
  Node* node = nullptr;
  bool isConstant = false;
  bool isInvalid = false;
  ENode* gateENode = nullptr; // ClockGate
  clockVal(Node* n) { node = n; }
  clockVal(int v) { isConstant = true; }
  clockVal(bool invalid) { isInvalid = invalid; }
};

std::map<Node*, clockVal*> clockMap;

clockVal* ENode::clockCompute() {
  if (width == 0) return new clockVal(0);
  if (nodePtr) {
    if (clockMap.find(nodePtr) != clockMap.end()) return clockMap[nodePtr];
    else return nodePtr->clockCompute();
  }

  clockVal* childVal = nullptr;
  clockVal* ret = nullptr;
  switch (opType) {
    case OP_ASUINT:
    case OP_ASSINT:
    case OP_ASCLOCK:
    case OP_ASASYNCRESET:
      ret = getChild(0)->clockCompute();
      break;
    case OP_BITS:
      Assert(values[0] == 0 && values[1] == 0, "bits %d %d", values[0], values[1]);
      ret = getChild(0)->clockCompute();
      break;
    case OP_INVALID:
      ret = new clockVal(true);
      ret->isInvalid = true;
      break;
    case OP_INT:
      ret = new clockVal(0);
      break;
    case OP_EQ:
      Assert(0, "unsupported equality-derived clock; cannot replace it with "
                "a constant clock without changing simulation semantics");
      break;
    case OP_OR:
      ret = new clockVal(false);
      ret->gateENode = this;
      break;
    case OP_AND:
      childVal = getChild(0)->clockCompute(); // first child
      ret = getChild(1)->clockCompute(); // second child
      if (childVal->node && ret->gateENode) {
        ret->node = childVal->node;
      } else if (childVal->gateENode && ret->node) {
        ret->gateENode = ret->gateENode;
      } else Panic();
      break;
    case OP_MUX: {
    case OP_WHEN:
      ENode* cond = getChild(0);
      ENode* neg = new ENode(OP_NOT);
      neg->addChild(cond);
      childVal = getChild(1)->clockCompute();
      Assert(childVal->isConstant || childVal->isInvalid || (childVal->node && childVal->node->type == NODE_INP), "invalid mux");
      ret = getChild(2)->clockCompute();
      Assert(ret->isConstant || ret->isInvalid || (ret->node && ret->node->type == NODE_INP), "invalid mux");
      ENode* first = nullptr, *second = nullptr;
      if (ret->isInvalid) {
        ret = childVal;
      } else if (!childVal->isInvalid) { // if childVal is invalid, ret is the clock
        if (!childVal->isConstant) { // null first
          if (childVal->gateENode) {
            first = new ENode(OP_AND);
            first->addChild(cond);
            first->addChild(childVal->gateENode);
          } else first = cond;
        }
        if (!ret->isConstant) { // null second
          if (ret->gateENode) {
            second = new ENode(OP_AND);
            second->addChild(neg);
            second->addChild(ret->gateENode);
          } else second = neg;
        }
        if (!first && !second) ret = new clockVal(0);
        else if (!first) ret->gateENode = second;
        else if (!second) ret->gateENode = first;
        else  {
          ENode* andEnode = new ENode(OP_OR);
          andEnode->addChild(first);
          andEnode->addChild(second);
          ret->gateENode = andEnode;
        }
      }
      break;
    }
    case OP_EXT_FUNC:
      // The body of a blackbox is not present in FIRRTL.  Treating its
      // generated clock as constant silently removes all clocked state.  It
      // requires explicit clock-domain support, not an alias/constant guess.
      Assert(0, "unsupported external-module clock source; cannot replace it "
                "with a constant clock without changing simulation semantics");
      break;
    default:
      Assert(0, "invalid op %d", opType);
  }
  return ret;
}

clockVal* Node::clockCompute() {
  if (clockMap.find(this) != clockMap.end()) return clockMap[this];
  if (type == NODE_INP || type == NODE_REG_SRC) {
    clockMap[this] = new clockVal(this);
    return clockMap[this];
  }
  Assert(assignTree.size() <= 1, "multiple clock assignment in %s", name.c_str());
  if (assignTree.size() != 0) {
    Assert(assignTree[0]->getRoot()->opType != OP_EXT_FUNC,
           "unsupported external-module clock source %s at line %d; "
           "cannot replace it with a constant clock without changing "
           "simulation semantics", name.c_str(), lineno);
    clockMap[this] = assignTree[0]->getRoot()->clockCompute();
  } else {
    Assert(0, "unsupported undriven clock %s at line %d; cannot replace it "
              "with a constant clock without changing simulation semantics",
              name.c_str(), lineno);
  }
  return clockMap[this];
}

Node* Node::clockAlias() {
  if (assignTree.size() != 1) return nullptr;
/* check if is when with invalid side */
  if (!assignTree[0]->getRoot()->getNode()) return nullptr;
  Assert(assignTree[0]->getRoot()->getChildNum() == 0, "alias clock %s to array %s\n", name.c_str(), assignTree[0]->getRoot()->getNode()->name.c_str());
  if (prev.size() != 1) return nullptr;
  return assignTree[0]->getRoot()->getNode();
}

void Node::setConstantZero(int w) {
  ENode* enode = allocIntEnode(w, "0");
  assignTree.clear();
  assignTree.push_back(new ExpTree(enode, this));
}
void Node::setConstantInfoZero(int w) {
  computeInfo = new valInfo();
  if (w > 0) computeInfo->width = w;
  else computeInfo->width = width;
  computeInfo->setConstantByStr("0");
  status = CONSTANT_NODE;
}

bool ExpTree::isReadTree() {
  std::stack<ENode*> s;
  s.push(getRoot());
  bool isRead = false;
  while (!s.empty()) {
    ENode* top = s.top();
    s.pop();
    if (top->opType == OP_READ_MEM) {
      isRead = true;
      break;
    } else {
      for (size_t i = 0; i < top->getChildNum(); i ++) {
        if (top->getChild(i)) s.push(top->getChild(i));
      }
    }
  }
  return isRead;
}

/* clock alias & clock constant analysis */
void graph::clockOptimize(std::map<std::string, Node*>& allSignals) {
  for (auto iter : allSignals) {
    Node* node = iter.second;
    // Only analyse clocks actually used for sequential evaluation.  A
    // Clock-typed output used as data is still an ordinary blackbox output;
    // evaluating its timing here would incorrectly classify the producer as
    // a clocked consumer or reject a clock which drives no state at all.
    bool consumer = node->type == NODE_REG_DST || node->type == NODE_EXT ||
                    node->type == NODE_READER || node->type == NODE_WRITER ||
                    node->type == NODE_READWRITER;
    if (!consumer || !node->clock) continue;
    Assert(!node->clock->isArray(), "clock %s is array", node->clock->name.c_str());
    node->clock->clockCompute();
  }

  for (auto iter : allSignals) {
    Node* node = iter.second;
    if (node->type == NODE_REG_DST) {
      Assert(clockMap.find(node->clock) != clockMap.end(), "%s: clock %s(%p) not found", node->name.c_str(), node->clock->name.c_str(), node->clock);
      clockVal* val = clockMap[node->clock];
      Assert(!val->gateENode || val->node, "invalid clock %s %d", node->clock->name.c_str(), node->clock->lineno);
      if (val->gateENode) {
        ENode* gate = new ENode(OP_WHEN);
        gate->width = node->width;
        gate->sign = node->sign;
        ENode* cond = val->gateENode->dup();
        gate->addChild(cond);
        gate->addChild(nullptr);
        gate->addChild(nullptr);
        for (ExpTree* tree : node->assignTree) {
          ENode* gateDup = gate->dup();
          gateDup->setChild(1, tree->getRoot());
          tree->setRoot(gateDup);
        }
      } else if (val->node) {
        if (node->clock->type != NODE_INP) {
          ENode* clockENode = new ENode(val->node);
          clockENode->width = val->node->width;
          node->clock->assignTree[0]->setRoot(clockENode);
        }
      } else {
        node->setConstantZero(0);
        node->regNext->setConstantZero(0);
        node->getSrc()->resetTree = nullptr;
        node->getSrc()->reset = ZERO_RESET;
      }
    } else if (node->type == NODE_EXT && node->clock) {
      clockVal* val = clockMap[node->clock];
      if (val->node) {
        ENode* clockENode = new ENode(val->node);
        clockENode->width = val->node->width;
        node->clock->assignTree[0]->setRoot(clockENode);
      } else {
        for (Node* member : node->member)
          member->setConstantZero(0);
      }
    } else if (node->type == NODE_READER || node->type == NODE_WRITER || node->type == NODE_READWRITER) {
      clockVal* val = nullptr;
      Node* clockMember = nullptr;
      if (node->type == NODE_READER || node->type == NODE_WRITER || node->type == NODE_READWRITER) {
        Assert(clockMap.find(node->clock) != clockMap.end(), "%s: clock %s not found", node->name.c_str(), node->clock->name.c_str());
        val = clockMap[node->clock];
        clockMember = node->clock;
      }
      if (val) {
        Assert(!val->gateENode || val->node, "invalid clock %s %d", node->clock->name.c_str(), node->clock->lineno);
        if (val->gateENode) {
          if (node->type == NODE_WRITER || node->type == NODE_READWRITER) {
            ENode* gate = new ENode(OP_WHEN);
            gate->width = node->width;
            gate->sign = node->sign;
            ENode* cond = val->gateENode->dup();
            gate->addChild(cond);
            gate->addChild(nullptr);
            gate->addChild(nullptr);
            for (ExpTree* tree : node->assignTree) {
              if (tree->isReadTree()) continue;
              ENode* gateDup = gate->dup();
              gateDup->setChild(1, tree->getRoot());
              tree->setRoot(gateDup);
            }
          }
        } else if (val->node) {
          if (!clockMember->assignTree.empty()) {
            ENode* clockENode = new ENode(val->node);
            clockENode->width = val->node->width;
            clockMember->assignTree[0]->setRoot(clockENode);
          }
        } else {
          for (Node* member : node->member)
            member->setConstantZero(0);
        }
      }
    }
  }
}
