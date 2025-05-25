//<---------QUANTA JS ENGINE - DOM IMPLEMENTATION--------->
// Stage 4: DOM Integration & Frameworks - DOM API Implementation
// Purpose: Basic DOM element creation, manipulation, and tree structure
// Max Lines: 5000 (Current: ~400)

#include "../include/dom.h"
#include <algorithm>
#include <sstream>
#include <chrono>

namespace Quanta {

//<---------EVENT IMPLEMENTATION--------->
Event::Event(const std::string& type, const std::string& target)
    : type_(type), target_(target) {
    // Set timestamp
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    timeStamp_ = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

JSValue Event::getProperty(const std::string& name) {
    if (name == "type") return JSValue(type_);
    if (name == "target") return JSValue(target_);
    if (name == "timeStamp") return JSValue(timeStamp_);
    if (name == "defaultPrevented") return JSValue(defaultPrevented_);
    if (name == "cancelled") return JSValue(cancelled_);
    
    return JSObject::getProperty(name);
}

void Event::setProperty(const std::string& name, const JSValue& value) {
    // Events are mostly read-only, but allow some properties
    JSObject::setProperty(name, value);
}

//<---------EVENT TARGET IMPLEMENTATION--------->
void EventTarget::addEventListener(const std::string& type, EventListener listener) {
    listeners_[type].push_back(listener);
}

void EventTarget::removeEventListener(const std::string& type) {
    listeners_.erase(type);
}

bool EventTarget::dispatchEvent(const Event& event) {
    auto it = listeners_.find(event.getType());
    if (it != listeners_.end()) {
        for (const auto& listener : it->second) {
            listener(event);
            if (event.isCancelled()) {
                return false;
            }
        }
    }
    return true;
}

//<---------DOM NODE IMPLEMENTATION--------->
DOMNode::DOMNode(DOMNodeType type, const std::string& name)
    : nodeType_(type), nodeName_(name) {
}

void DOMNode::appendChild(std::shared_ptr<DOMNode> child) {
    if (child->parent_) {
        // Remove from current parent first
        auto& siblings = child->parent_->children_;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
    }
    
    child->parent_ = this;
    children_.push_back(child);
}

void DOMNode::insertBefore(std::shared_ptr<DOMNode> newChild, std::shared_ptr<DOMNode> referenceChild) {
    if (newChild->parent_) {
        // Remove from current parent first
        auto& siblings = newChild->parent_->children_;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), newChild), siblings.end());
    }
    
    auto it = std::find(children_.begin(), children_.end(), referenceChild);
    if (it != children_.end()) {
        children_.insert(it, newChild);
        newChild->parent_ = this;
    } else {
        appendChild(newChild); // Reference child not found, append at end
    }
}

void DOMNode::removeChild(std::shared_ptr<DOMNode> child) {
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

JSValue DOMNode::getProperty(const std::string& name) {
    if (name == "nodeType") return JSValue(static_cast<double>(nodeType_));
    if (name == "nodeName") return JSValue(nodeName_);
    if (name == "textContent") return JSValue(textContent_);
    if (name == "parentNode") {
        if (parent_) {
            // In a real implementation, we'd return a JS wrapper
            return JSValue(std::string("DOMNode"));
        }
        return JSValue();
    }
    if (name == "childNodes") {
        // In a real implementation, we'd return a NodeList
        return JSValue(static_cast<double>(children_.size()));
    }
    
    return JSObject::getProperty(name);
}

void DOMNode::setProperty(const std::string& name, const JSValue& value) {
    if (name == "textContent") {
        if (std::holds_alternative<std::string>(value)) {
            textContent_ = std::get<std::string>(value);
            return;
        }
    }
    
    JSObject::setProperty(name, value);
}

std::string DOMNode::toString() const {
    return nodeName_ + " (" + std::to_string(children_.size()) + " children)";
}

std::string DOMNode::toHTML() const {
    if (nodeType_ == DOMNodeType::TEXT) {
        return escapeHTML(textContent_);
    }
    return textContent_;
}

//<---------DOM ELEMENT IMPLEMENTATION--------->
DOMElement::DOMElement(const std::string& tagName)
    : DOMNode(DOMNodeType::ELEMENT, tagName), tagName_(tagName) {
}

std::string DOMElement::getId() const {
    return getAttribute("id");
}

void DOMElement::setId(const std::string& id) {
    setAttribute("id", id);
}

std::string DOMElement::getClassName() const {
    return getAttribute("class");
}

void DOMElement::setClassName(const std::string& className) {
    setAttribute("class", className);
}

std::string DOMElement::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    return (it != attributes_.end()) ? it->second : "";
}

void DOMElement::setAttribute(const std::string& name, const std::string& value) {
    attributes_[name] = value;
}

void DOMElement::removeAttribute(const std::string& name) {
    attributes_.erase(name);
}

bool DOMElement::hasAttribute(const std::string& name) const {
    return attributes_.find(name) != attributes_.end();
}

void DOMElement::setStyle(const std::string& property, const std::string& value) {
    styles_[property] = value;
}

std::string DOMElement::getStyle(const std::string& property) const {
    auto it = styles_.find(property);
    return (it != styles_.end()) ? it->second : "";
}

std::shared_ptr<DOMElement> DOMElement::getElementById(const std::string& id) {
    if (getId() == id) {
        // Create a shared_ptr from this element
        if (parent_) {
            for (const auto& child : parent_->getChildNodes()) {
                if (child.get() == this) {
                    return std::static_pointer_cast<DOMElement>(child);
                }
            }
        }
    }
    
    for (const auto& child : children_) {
        if (auto element = std::dynamic_pointer_cast<DOMElement>(child)) {
            if (auto found = element->getElementById(id)) {
                return found;
            }
        }
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<DOMElement>> DOMElement::getElementsByTagName(const std::string& tagName) {
    std::vector<std::shared_ptr<DOMElement>> result;
    
    if (tagName_ == tagName || tagName == "*") {
        // Find this element in parent's children to get shared_ptr
        if (parent_) {
            for (const auto& child : parent_->getChildNodes()) {
                if (child.get() == this) {
                    result.push_back(std::static_pointer_cast<DOMElement>(child));
                    break;
                }
            }
        }
    }
    
    for (const auto& child : children_) {
        if (auto element = std::dynamic_pointer_cast<DOMElement>(child)) {
            auto childResults = element->getElementsByTagName(tagName);
            result.insert(result.end(), childResults.begin(), childResults.end());
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<DOMElement>> DOMElement::getElementsByClassName(const std::string& className) {
    std::vector<std::shared_ptr<DOMElement>> result;
    
    std::string myClass = getClassName();
    if (myClass.find(className) != std::string::npos) {
        // Find this element in parent's children to get shared_ptr
        if (parent_) {
            for (const auto& child : parent_->children_) {
                if (child.get() == this) {
                    result.push_back(std::static_pointer_cast<DOMElement>(child));
                    break;
                }
            }
        }
    }
    
    for (const auto& child : children_) {
        if (auto element = std::dynamic_pointer_cast<DOMElement>(child)) {
            auto childResults = element->getElementsByClassName(className);
            result.insert(result.end(), childResults.begin(), childResults.end());
        }
    }
      return result;
}

JSValue DOMElement::getProperty(const std::string& name) {
    if (name == "tagName") return JSValue(tagName_);
    if (name == "id") return JSValue(getId());
    if (name == "className") return JSValue(getClassName());
    if (name == "innerHTML") return JSValue(toHTML());
    
    // Check attributes
    if (hasAttribute(name)) {
        return JSValue(getAttribute(name));
    }
    
    // Check styles
    if (name.substr(0, 5) == "style" && name.length() > 5) {
        std::string styleProp = name.substr(5);
        if (!styleProp.empty()) {
            styleProp[0] = std::tolower(styleProp[0]);
            return JSValue(getStyle(styleProp));
        }
    }
    
    return DOMNode::getProperty(name);
}

void DOMElement::setProperty(const std::string& name, const JSValue& value) {
    if (name == "id" && std::holds_alternative<std::string>(value)) {
        setId(std::get<std::string>(value));
        return;
    }
    
    if (name == "className" && std::holds_alternative<std::string>(value)) {
        setClassName(std::get<std::string>(value));
        return;
    }
    
    if (name == "innerHTML" && std::holds_alternative<std::string>(value)) {
        // In a real implementation, we'd parse the HTML
        setTextContent(std::get<std::string>(value));
        return;
    }
    
    // Handle style properties
    if (name.substr(0, 5) == "style" && name.length() > 5 && std::holds_alternative<std::string>(value)) {
        std::string styleProp = name.substr(5);
        if (!styleProp.empty()) {
            styleProp[0] = std::tolower(styleProp[0]);
            setStyle(styleProp, std::get<std::string>(value));
            return;
        }
    }
    
    // Default to setting as attribute
    if (std::holds_alternative<std::string>(value)) {
        setAttribute(name, std::get<std::string>(value));
        return;
    }
    
    DOMNode::setProperty(name, value);
}

std::string DOMElement::toHTML() const {
    std::ostringstream html;
    
    html << "<" << tagName_;
    
    // Add attributes
    for (const auto& attr : attributes_) {
        html << " " << attr.first << "=\"" << escapeHTML(attr.second) << "\"";
    }
    
    // Add styles
    if (!styles_.empty()) {
        html << " style=\"";
        for (const auto& style : styles_) {
            html << style.first << ":" << style.second << ";";
        }
        html << "\"";
    }
    
    html << ">";
    
    // Add content
    if (!textContent_.empty()) {
        html << escapeHTML(textContent_);
    }
    
    // Add children
    for (const auto& child : children_) {
        html << child->toHTML();
    }
    
    html << "</" << tagName_ << ">";
    
    return html.str();
}

//<---------DOM DOCUMENT IMPLEMENTATION--------->
DOMDocument::DOMDocument() : DOMNode(DOMNodeType::DOCUMENT, "#document") {
    // Create basic document structure
    documentElement_ = std::make_shared<DOMElement>("html");
    head_ = std::make_shared<DOMElement>("head");
    body_ = std::make_shared<DOMElement>("body");
    
    documentElement_->appendChild(head_);
    documentElement_->appendChild(body_);
    appendChild(documentElement_);
}

std::shared_ptr<DOMElement> DOMDocument::createElement(const std::string& tagName) {
    return std::make_shared<DOMElement>(tagName);
}

std::shared_ptr<DOMNode> DOMDocument::createTextNode(const std::string& data) {
    auto textNode = std::make_shared<DOMNode>(DOMNodeType::TEXT, "#text");
    textNode->setTextContent(data);
    return textNode;
}

std::shared_ptr<DOMNode> DOMDocument::createComment(const std::string& data) {
    auto commentNode = std::make_shared<DOMNode>(DOMNodeType::COMMENT, "#comment");
    commentNode->setTextContent(data);
    return commentNode;
}

std::shared_ptr<DOMElement> DOMDocument::getElementById(const std::string& id) {
    return documentElement_->getElementById(id);
}

std::vector<std::shared_ptr<DOMElement>> DOMDocument::getElementsByTagName(const std::string& tagName) {
    return documentElement_->getElementsByTagName(tagName);
}

std::vector<std::shared_ptr<DOMElement>> DOMDocument::getElementsByClassName(const std::string& className) {
    return documentElement_->getElementsByClassName(className);
}

JSValue DOMDocument::getProperty(const std::string& name) {
    if (name == "documentElement") {
        return JSValue(std::string("HTMLElement"));
    }
    if (name == "head") {
        return JSValue(std::string("HTMLHeadElement"));
    }
    if (name == "body") {
        return JSValue(std::string("HTMLBodyElement"));
    }
    
    return DOMNode::getProperty(name);
}

void DOMDocument::setProperty(const std::string& name, const JSValue& value) {
    DOMNode::setProperty(name, value);
}

//<---------UTILITY FUNCTIONS--------->
std::shared_ptr<DOMDocument> createDocument() {
    return std::make_shared<DOMDocument>();
}

std::string escapeHTML(const std::string& text) {
    std::string result;
    result.reserve(text.length() * 1.1); // Reserve some extra space
    
    for (char c : text) {
        switch (c) {
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '&': result += "&amp;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break;
            default: result += c; break;
        }
    }
    
    return result;
}

std::string unescapeHTML(const std::string& html) {
    std::string result = html;
    
    // Simple unescape (real implementation would be more robust)
    size_t pos = 0;
    while ((pos = result.find("&lt;", pos)) != std::string::npos) {
        result.replace(pos, 4, "<");
        pos += 1;
    }
    
    pos = 0;
    while ((pos = result.find("&gt;", pos)) != std::string::npos) {
        result.replace(pos, 4, ">");
        pos += 1;
    }
    
    pos = 0;
    while ((pos = result.find("&amp;", pos)) != std::string::npos) {
        result.replace(pos, 5, "&");
        pos += 1;
    }
    
    pos = 0;
    while ((pos = result.find("&quot;", pos)) != std::string::npos) {
        result.replace(pos, 6, "\"");
        pos += 1;
    }
    
    pos = 0;
    while ((pos = result.find("&#39;", pos)) != std::string::npos) {
        result.replace(pos, 5, "'");
        pos += 1;
    }
    
    return result;
}

//<---------DOM GLOBAL IMPLEMENTATION--------->
DOMGlobal::DOMGlobal(std::shared_ptr<DOMDocument> document) : document_(document) {
}

JSValue DOMGlobal::getProperty(const std::string& name) {
    if (name == "document") {
        return JSValue(std::string("Document"));
    }
    if (name == "window") {
        return JSValue(std::string("Window"));
    }
    
    return JSObject::getProperty(name);
}

void DOMGlobal::setProperty(const std::string& name, const JSValue& value) {
    JSObject::setProperty(name, value);
}

} // namespace Quanta
