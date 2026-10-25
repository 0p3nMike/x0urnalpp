/*
 * X0urnal++ -- the notebook tree. See NotebookModel.h for the format.
 *
 * @license GNU GPLv2 or later
 */

#include "NotebookModel.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <glib.h>  // g_get_host_name, g_getenv, g_get_real_time, g_random_int_range

namespace x0urnal {

NotebookModel::NotebookModel(): rootNode(std::make_unique<NotebookNode>()) {
    rootNode->kind = NotebookNode::Kind::Chapter;
    rootNode->id = "root";
    rootNode->title = "Notebook";
}

NotebookModel::~NotebookModel() = default;

namespace {

void collectSections(const NotebookNode& node, std::vector<const NotebookNode*>& out) {
    for (const auto& child: node.children) {
        if (child->isChapter()) {
            collectSections(*child, out);
        } else {
            out.push_back(child.get());
        }
    }
}

NotebookNode* findIn(NotebookNode& node, const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (auto& child: node.children) {
        if (NotebookNode* hit = findIn(*child, id)) {
            return hit;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<const NotebookNode*> NotebookModel::sections() const {
    std::vector<const NotebookNode*> out;
    collectSections(*rootNode, out);
    return out;
}

NotebookNode* NotebookModel::findById(const std::string& id) { return findIn(*rootNode, id); }

namespace {

std::string base36(uint64_t v) {
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out;
    do {
        out.insert(out.begin(), digits[v % 36]);
        v /= 36;
    } while (v);
    return out;
}

}  // namespace

std::string NotebookModel::deviceName() {
    /* The same rule as book-sync's (book-sync/README.md "Ids"), so a page made
       here and a commit made by the sync tool name the machine the same way:
       BOOK_SYNC_DEVICE if it is set, else the hostname's first label, lowercased. */
    std::string host;
    if (const char* forced = g_getenv("BOOK_SYNC_DEVICE"); forced && *forced) {
        host = forced;
    } else {
        host = g_get_host_name();
        host = host.substr(0, host.find('.'));
    }
    std::string out;
    for (char c: host) {
        const auto lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out += std::isalnum(static_cast<unsigned char>(lc)) ? lc : '-';
    }
    return out.empty() ? "device" : out;
}

std::string NotebookModel::newId() {
    /* See the header: unique across devices without asking anyone. Two pages made
       in the same millisecond on one machine differ in the random tail, and the
       loop catches the one-in-1.7-million case where they do not. */
    const std::string device = deviceName();
    std::string candidate;
    do {
        const auto ms = static_cast<uint64_t>(g_get_real_time() / 1000);
        std::string tail = base36(static_cast<uint64_t>(g_random_int_range(0, 36 * 36 * 36 * 36)));
        tail.insert(tail.begin(), 4 - std::min<size_t>(4, tail.size()), '0');
        candidate = device + "-" + base36(ms) + "-" + tail;
    } while (findById(candidate) != nullptr);
    return candidate;
}

NotebookNode* NotebookModel::add(NotebookNode::Kind kind, const std::string& title, NotebookNode* parent) {
    NotebookNode* target = parent ? parent : rootNode.get();
    auto node = std::make_unique<NotebookNode>();
    node->kind = kind;
    node->id = newId();
    node->title = title;
    if (kind == NotebookNode::Kind::Section) {
        /* The file name is derived from the id, not the title: titles are
           renamed and may contain anything a user can type, and a path that
           changes when a title changes would orphan the file. */
        node->sectionPath = "sections/" + node->id + ".xopp";
    }
    NotebookNode* raw = node.get();
    target->children.push_back(std::move(node));
    return raw;
}

bool NotebookModel::remove(const std::string& id) {
    if (id == "root") {
        return false;  // the root is the notebook itself
    }
    /* Depth-first search for the OWNER of the node, since the child is held by
       unique_ptr in its parent's vector. */
    std::vector<NotebookNode*> stack{rootNode.get()};
    while (!stack.empty()) {
        NotebookNode* node = stack.back();
        stack.pop_back();
        auto it = std::find_if(node->children.begin(), node->children.end(),
                               [&id](const std::unique_ptr<NotebookNode>& c) { return c->id == id; });
        if (it != node->children.end()) {
            node->children.erase(it);
            return true;
        }
        for (auto& child: node->children) {
            stack.push_back(child.get());
        }
    }
    return false;
}

NotebookNode* NotebookModel::parentOf(const std::string& id) {
    std::vector<NotebookNode*> stack{rootNode.get()};
    while (!stack.empty()) {
        NotebookNode* node = stack.back();
        stack.pop_back();
        for (auto& child: node->children) {
            if (child->id == id) {
                return node;
            }
            stack.push_back(child.get());
        }
    }
    return nullptr;
}

bool NotebookModel::move(const std::string& id, NotebookNode* newParent, int index) {
    NotebookNode* target = newParent ? newParent : rootNode.get();
    NotebookNode* oldParent = parentOf(id);
    if (!oldParent || id == "root" || (target != rootNode.get() && !target->isChapter())) {
        return false;
    }
    auto it = std::find_if(oldParent->children.begin(), oldParent->children.end(),
                           [&id](const std::unique_ptr<NotebookNode>& c) { return c->id == id; });
    /* Not into itself or anything under it: walk up from the target. */
    for (NotebookNode* up = target; up && up != rootNode.get(); up = parentOf(up->id)) {
        if (up == it->get()) {
            return false;
        }
    }
    std::unique_ptr<NotebookNode> node = std::move(*it);
    const auto oldIndex = static_cast<int>(it - oldParent->children.begin());
    oldParent->children.erase(it);
    if (target == oldParent && index > oldIndex) {
        --index;  // the indices after it shifted down by one when it left
    }
    const int n = static_cast<int>(target->children.size());
    if (index < 0 || index > n) {
        index = n;
    }
    target->children.insert(target->children.begin() + index, std::move(node));
    return true;
}

}  // namespace x0urnal
