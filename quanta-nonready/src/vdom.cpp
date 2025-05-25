//<---------QUANTA JS ENGINE - VIRTUAL DOM IMPLEMENTATION--------->
// Stage 4: DOM Integration & Frameworks - Virtual DOM System
// Purpose: Virtual DOM implementation for efficient DOM manipulation and framework support
// Max Lines: 5000 (Current: ~450)

#include "../include/vdom.h"
#include <algorithm>
#include <sstream>

namespace Quanta {

//<---------VIRTUAL ELEMENT IMPLEMENTATION--------->
VElement::VElement(const std::string& tagName) : VNode(VNodeType::ELEMENT), tagName_(tagName) {
}

void VElement::setProp(const std::string& name, const JSValue& value) {
    props_[name] = value;
}

JSValue VElement::getProp(const std::string& name) const {
    auto it = props_.find(name);
    return (it != props_.end()) ? it->second : JSValue();
}

bool VElement::hasProp(const std::string& name) const {
    return props_.find(name) != props_.end();
}

void VElement::addChild(std::shared_ptr<VNode> child) {
    children_.push_back(child);
}

void VElement::setChildren(const std::vector<std::shared_ptr<VNode>>& children) {
    children_ = children;
}

std::shared_ptr<VNode> VElement::clone() const {
    auto cloned = std::make_shared<VElement>(tagName_);
    cloned->props_ = props_;
    
    for (const auto& child : children_) {
        cloned->children_.push_back(child->clone());
    }
    
    return cloned;
}

bool VElement::equals(const VNode& other) const {
    if (other.getType() != VNodeType::ELEMENT) return false;
    
    const VElement& otherElement = static_cast<const VElement&>(other);
    
    if (tagName_ != otherElement.tagName_) return false;
    if (props_.size() != otherElement.props_.size()) return false;
    if (children_.size() != otherElement.children_.size()) return false;
    
    // Compare props
    for (const auto& prop : props_) {
        auto it = otherElement.props_.find(prop.first);
        if (it == otherElement.props_.end() || it->second != prop.second) {
            return false;
        }
    }
    
    // Compare children
    for (size_t i = 0; i < children_.size(); ++i) {
        if (!children_[i]->equals(*otherElement.children_[i])) {
            return false;
        }
    }
    
    return true;
}

std::string VElement::toString() const {
    std::ostringstream oss;
    oss << "<" << tagName_;
    
    for (const auto& prop : props_) {
        oss << " " << prop.first << "=\"";
        // Simple value conversion
        if (std::holds_alternative<std::string>(prop.second)) {
            oss << std::get<std::string>(prop.second);
        } else if (std::holds_alternative<double>(prop.second)) {
            oss << std::get<double>(prop.second);
        } else if (std::holds_alternative<bool>(prop.second)) {
            oss << (std::get<bool>(prop.second) ? "true" : "false");
        }
        oss << "\"";
    }
    
    if (children_.empty()) {
        oss << " />";
    } else {
        oss << ">";
        for (const auto& child : children_) {
            oss << child->toString();
        }
        oss << "</" << tagName_ << ">";
    }
    
    return oss.str();
}

//<---------VIRTUAL TEXT IMPLEMENTATION--------->
VText::VText(const std::string& text) : VNode(VNodeType::TEXT), text_(text) {
}

std::shared_ptr<VNode> VText::clone() const {
    return std::make_shared<VText>(text_);
}

bool VText::equals(const VNode& other) const {
    if (other.getType() != VNodeType::TEXT) return false;
    
    const VText& otherText = static_cast<const VText&>(other);
    return text_ == otherText.text_;
}

std::string VText::toString() const {
    return text_;
}

//<---------COMPONENT DEFINITION IMPLEMENTATION--------->
ComponentDefinition::ComponentDefinition(const std::string& name, ComponentFunction renderFn)
    : name_(name), renderFunction_(renderFn) {
}

std::shared_ptr<VNode> ComponentDefinition::render(const std::unordered_map<std::string, JSValue>& props) const {
    return renderFunction_(props);
}

//<---------VIRTUAL COMPONENT IMPLEMENTATION--------->
VComponent::VComponent(const std::string& componentName) 
    : VNode(VNodeType::COMPONENT), componentName_(componentName) {
}

void VComponent::setProp(const std::string& name, const JSValue& value) {
    props_[name] = value;
}

JSValue VComponent::getProp(const std::string& name) const {
    auto it = props_.find(name);
    return (it != props_.end()) ? it->second : JSValue();
}

std::shared_ptr<VNode> VComponent::clone() const {
    auto cloned = std::make_shared<VComponent>(componentName_);
    cloned->props_ = props_;
    return cloned;
}

bool VComponent::equals(const VNode& other) const {
    if (other.getType() != VNodeType::COMPONENT) return false;
    
    const VComponent& otherComponent = static_cast<const VComponent&>(other);
    
    if (componentName_ != otherComponent.componentName_) return false;
    if (props_.size() != otherComponent.props_.size()) return false;
    
    // Compare props
    for (const auto& prop : props_) {
        auto it = otherComponent.props_.find(prop.first);
        if (it == otherComponent.props_.end() || it->second != prop.second) {
            return false;
        }
    }
    
    return true;
}

std::string VComponent::toString() const {
    std::ostringstream oss;
    oss << "<" << componentName_;
    
    for (const auto& prop : props_) {
        oss << " " << prop.first << "=\"";
        if (std::holds_alternative<std::string>(prop.second)) {
            oss << std::get<std::string>(prop.second);
        } else if (std::holds_alternative<double>(prop.second)) {
            oss << std::get<double>(prop.second);
        } else if (std::holds_alternative<bool>(prop.second)) {
            oss << (std::get<bool>(prop.second) ? "true" : "false");
        }
        oss << "\"";
    }
    
    oss << " />";
    return oss.str();
}

//<---------VDOM DIFFER IMPLEMENTATION--------->
std::vector<Patch> VDOMDiffer::diff(std::shared_ptr<VNode> oldNode, std::shared_ptr<VNode> newNode) {
    std::vector<Patch> patches;
    
    if (!oldNode && !newNode) {
        return patches;
    }
    
    if (!oldNode && newNode) {
        // Create new node
        Patch patch;
        patch.type = PatchType::CREATE;
        patch.newNode = newNode;
        patches.push_back(patch);
        return patches;
    }
    
    if (oldNode && !newNode) {
        // Remove node
        Patch patch;
        patch.type = PatchType::REMOVE;
        patch.oldNode = oldNode;
        patches.push_back(patch);
        return patches;
    }
    
    if (oldNode->getType() != newNode->getType()) {
        // Replace node
        Patch patch;
        patch.type = PatchType::REPLACE;
        patch.oldNode = oldNode;
        patch.newNode = newNode;
        patches.push_back(patch);
        return patches;
    }
    
    if (oldNode->getType() == VNodeType::TEXT) {
        VText* oldText = static_cast<VText*>(oldNode.get());
        VText* newText = static_cast<VText*>(newNode.get());
        
        if (oldText->getText() != newText->getText()) {
            Patch patch;
            patch.type = PatchType::UPDATE_TEXT;
            patch.newText = newText->getText();
            patches.push_back(patch);
        }
    } else if (oldNode->getType() == VNodeType::ELEMENT) {
        VElement* oldElement = static_cast<VElement*>(oldNode.get());
        VElement* newElement = static_cast<VElement*>(newNode.get());
        
        if (oldElement->getTagName() != newElement->getTagName()) {
            // Different tag names, replace
            Patch patch;
            patch.type = PatchType::REPLACE;
            patch.oldNode = oldNode;
            patch.newNode = newNode;
            patches.push_back(patch);
            return patches;
        }
        
        // Check for prop changes
        auto propChanges = diffProps(oldElement->getProps(), newElement->getProps());
        if (!propChanges.empty()) {
            Patch patch;
            patch.type = PatchType::UPDATE_PROPS;
            patch.propChanges = propChanges;
            patches.push_back(patch);
        }
        
        // Diff children
        auto childPatches = diffChildren(oldElement->getChildren(), newElement->getChildren());
        patches.insert(patches.end(), childPatches.begin(), childPatches.end());
    }
    
    return patches;
}

std::vector<Patch> VDOMDiffer::diffChildren(
    const std::vector<std::shared_ptr<VNode>>& oldChildren,
    const std::vector<std::shared_ptr<VNode>>& newChildren) {
    
    std::vector<Patch> patches;
    
    size_t oldSize = oldChildren.size();
    size_t newSize = newChildren.size();
    size_t minSize = std::min(oldSize, newSize);
    
    // Diff existing children
    for (size_t i = 0; i < minSize; ++i) {
        auto childPatches = diff(oldChildren[i], newChildren[i]);
        patches.insert(patches.end(), childPatches.begin(), childPatches.end());
    }
    
    // Handle additional new children
    for (size_t i = minSize; i < newSize; ++i) {
        Patch patch;
        patch.type = PatchType::CREATE;
        patch.newNode = newChildren[i];
        patch.index = i;
        patches.push_back(patch);
    }
    
    // Handle removed children
    for (size_t i = minSize; i < oldSize; ++i) {
        Patch patch;
        patch.type = PatchType::REMOVE;
        patch.oldNode = oldChildren[i];
        patch.index = i;
        patches.push_back(patch);
    }
    
    return patches;
}

std::unordered_map<std::string, JSValue> VDOMDiffer::diffProps(
    const std::unordered_map<std::string, JSValue>& oldProps,
    const std::unordered_map<std::string, JSValue>& newProps) {
    
    std::unordered_map<std::string, JSValue> changes;
    
    // Check for changed or new props
    for (const auto& newProp : newProps) {
        auto it = oldProps.find(newProp.first);
        if (it == oldProps.end() || it->second != newProp.second) {
            changes[newProp.first] = newProp.second;
        }
    }
    
    // Check for removed props (set to null/undefined)
    for (const auto& oldProp : oldProps) {
        if (newProps.find(oldProp.first) == newProps.end()) {
            changes[oldProp.first] = JSValue(); // null/undefined
        }
    }
    
    return changes;
}

//<---------VDOM RENDERER IMPLEMENTATION--------->
VDOMRenderer::VDOMRenderer(std::shared_ptr<DOMDocument> document) : document_(document) {
}

std::shared_ptr<DOMNode> VDOMRenderer::render(std::shared_ptr<VNode> vnode) {
    if (!vnode) return nullptr;
    
    switch (vnode->getType()) {
        case VNodeType::TEXT: {
            VText* vtext = static_cast<VText*>(vnode.get());
            return createTextNode(*vtext);
        }
        case VNodeType::ELEMENT: {
            VElement* velement = static_cast<VElement*>(vnode.get());
            return createElement(*velement);
        }
        case VNodeType::COMPONENT: {
            // For components, we'd need to resolve them first
            // This is a simplified version
            return document_->createTextNode("Component");
        }
        default:
            return nullptr;
    }
}

void VDOMRenderer::patch(std::shared_ptr<DOMNode> domNode, const std::vector<Patch>& patches) {
    for (const auto& patch : patches) {
        applyPatch(domNode, patch);
    }
}

void VDOMRenderer::update(std::shared_ptr<VNode> oldVNode, std::shared_ptr<VNode> newVNode, std::shared_ptr<DOMNode> container) {
    auto patches = VDOMDiffer::diff(oldVNode, newVNode);
    patch(container, patches);
}

std::shared_ptr<DOMNode> VDOMRenderer::createElement(const VElement& vElement) {
    auto element = document_->createElement(vElement.getTagName());
    
    // Set properties
    for (const auto& prop : vElement.getProps()) {
        if (std::holds_alternative<std::string>(prop.second)) {
            element->setAttribute(prop.first, std::get<std::string>(prop.second));
        } else if (std::holds_alternative<double>(prop.second)) {
            element->setAttribute(prop.first, std::to_string(std::get<double>(prop.second)));
        } else if (std::holds_alternative<bool>(prop.second)) {
            element->setAttribute(prop.first, std::get<bool>(prop.second) ? "true" : "false");
        }
    }
    
    // Add children
    for (const auto& child : vElement.getChildren()) {
        auto childNode = render(child);
        if (childNode) {
            element->appendChild(childNode);
        }
    }
    
    return element;
}

std::shared_ptr<DOMNode> VDOMRenderer::createTextNode(const VText& vText) {
    return document_->createTextNode(vText.getText());
}

void VDOMRenderer::updateElement(std::shared_ptr<DOMElement> element, const Patch& patch) {
    if (patch.type == PatchType::UPDATE_PROPS) {
        for (const auto& change : patch.propChanges) {
            if (std::holds_alternative<std::string>(change.second)) {
                element->setAttribute(change.first, std::get<std::string>(change.second));
            } else {
                element->removeAttribute(change.first);
            }
        }
    }
}

void VDOMRenderer::applyPatch(std::shared_ptr<DOMNode> domNode, const Patch& patch) {
    switch (patch.type) {
        case PatchType::UPDATE_PROPS: {
            if (auto element = std::dynamic_pointer_cast<DOMElement>(domNode)) {
                updateElement(element, patch);
            }
            break;
        }
        case PatchType::UPDATE_TEXT: {
            domNode->setTextContent(patch.newText);
            break;
        }
        case PatchType::CREATE: {
            auto newDomNode = render(patch.newNode);
            if (newDomNode) {
                domNode->appendChild(newDomNode);
            }
            break;
        }
        case PatchType::REPLACE: {
            auto newDomNode = render(patch.newNode);
            if (newDomNode && domNode->getParentNode()) {
                // Replace implementation would go here
                // For now, just append the new node
                domNode->getParentNode()->appendChild(newDomNode);
            }
            break;
        }
        case PatchType::REMOVE: {
            if (domNode->getParentNode()) {
                // Remove implementation would go here
            }
            break;
        }
        default:
            break;
    }
}

//<---------COMPONENT REGISTRY IMPLEMENTATION--------->
ComponentRegistry& ComponentRegistry::getInstance() {
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::registerComponent(const std::string& name, ComponentFunction renderFn) {
    components_[name] = std::make_unique<ComponentDefinition>(name, renderFn);
}

ComponentDefinition* ComponentRegistry::getComponent(const std::string& name) {
    auto it = components_.find(name);
    return (it != components_.end()) ? it->second.get() : nullptr;
}

bool ComponentRegistry::hasComponent(const std::string& name) const {
    return components_.find(name) != components_.end();
}

std::vector<std::string> ComponentRegistry::getComponentNames() const {
    std::vector<std::string> names;
    for (const auto& component : components_) {
        names.push_back(component.first);
    }
    return names;
}

//<---------VIRTUAL DOM FACTORY FUNCTIONS--------->
std::shared_ptr<VElement> createElement(const std::string& tagName) {
    return std::make_shared<VElement>(tagName);
}

std::shared_ptr<VElement> createElement(const std::string& tagName, const std::unordered_map<std::string, JSValue>& props) {
    auto element = std::make_shared<VElement>(tagName);
    for (const auto& prop : props) {
        element->setProp(prop.first, prop.second);
    }
    return element;
}

std::shared_ptr<VElement> createElement(const std::string& tagName, const std::unordered_map<std::string, JSValue>& props, const std::vector<std::shared_ptr<VNode>>& children) {
    auto element = createElement(tagName, props);
    element->setChildren(children);
    return element;
}

std::shared_ptr<VText> createTextNode(const std::string& text) {
    return std::make_shared<VText>(text);
}

std::shared_ptr<VComponent> createComponent(const std::string& componentName) {
    return std::make_shared<VComponent>(componentName);
}

std::shared_ptr<VComponent> createComponent(const std::string& componentName, const std::unordered_map<std::string, JSValue>& props) {
    auto component = std::make_shared<VComponent>(componentName);
    for (const auto& prop : props) {
        component->setProp(prop.first, prop.second);
    }
    return component;
}

//<---------VNODE BUILDER IMPLEMENTATION--------->
VNodeBuilder::VNodeBuilder(const std::string& tagName) : element_(std::make_shared<VElement>(tagName)) {
}

VNodeBuilder& VNodeBuilder::prop(const std::string& name, const JSValue& value) {
    element_->setProp(name, value);
    return *this;
}

VNodeBuilder& VNodeBuilder::child(std::shared_ptr<VNode> child) {
    element_->addChild(child);
    return *this;
}

VNodeBuilder& VNodeBuilder::text(const std::string& text) {
    element_->addChild(std::make_shared<VText>(text));
    return *this;
}

VNodeBuilder& VNodeBuilder::children(const std::vector<std::shared_ptr<VNode>>& children) {
    element_->setChildren(children);
    return *this;
}

std::shared_ptr<VElement> VNodeBuilder::build() {
    return element_;
}

//<---------VDOM GLOBAL IMPLEMENTATION--------->
VDOMGlobal::VDOMGlobal() : registry_(ComponentRegistry::getInstance()) {
}

JSValue VDOMGlobal::getProperty(const std::string& name) {
    if (name == "createElement") {
        return JSValue(std::string("function"));
    }
    if (name == "createTextNode") {
        return JSValue(std::string("function"));
    }
    if (name == "createComponent") {
        return JSValue(std::string("function"));
    }
    
    return JSObject::getProperty(name);
}

void VDOMGlobal::setProperty(const std::string& name, const JSValue& value) {
    JSObject::setProperty(name, value);
}

} // namespace Quanta
