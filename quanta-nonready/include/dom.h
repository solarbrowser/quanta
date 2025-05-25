//<---------QUANTA JS ENGINE - DOM INTEGRATION--------->
// Stage 4: DOM Integration & Frameworks - DOM API System
// Purpose: Basic DOM element creation, manipulation, and tree structure
// Max Lines: 5000 (Current: ~150)

#ifndef QUANTA_DOM_H
#define QUANTA_DOM_H

#include "runtime_objects.h"
#include "error.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace Quanta {

//<---------DOM NODE TYPES--------->
enum class DOMNodeType {
    ELEMENT,
    TEXT,
    COMMENT,
    DOCUMENT,
    DOCUMENT_FRAGMENT
};

//<---------FORWARD DECLARATIONS--------->
class DOMNode;
class DOMElement;
class DOMDocument;
class EventTarget;
class Event;

//<---------DOM EVENT SYSTEM--------->
class Event : public JSObject {
public:
    Event(const std::string& type, const std::string& target = "");
    
    std::string getType() const { return type_; }
    std::string getTarget() const { return target_; }
    bool isCancelled() const { return cancelled_; }
    
    void preventDefault() { defaultPrevented_ = true; }    void stopPropagation() { cancelled_ = true; }
    
    // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    std::string type_;
    std::string target_;
    bool cancelled_ = false;
    bool defaultPrevented_ = false;
    double timeStamp_;
};

//<---------EVENT LISTENER SYSTEM--------->
using EventListener = std::function<void(const Event&)>;

class EventTarget {
public:
    virtual ~EventTarget() = default;
    
    void addEventListener(const std::string& type, EventListener listener);
    void removeEventListener(const std::string& type);
    bool dispatchEvent(const Event& event);
    
protected:
    std::unordered_map<std::string, std::vector<EventListener>> listeners_;
};

//<---------DOM NODE BASE CLASS--------->
class DOMNode : public JSObject, public EventTarget {
public:
    DOMNode(DOMNodeType type, const std::string& name = "");
    virtual ~DOMNode() = default;
    
    // Node properties
    DOMNodeType getNodeType() const { return nodeType_; }
    std::string getNodeName() const { return nodeName_; }
    std::string getTextContent() const { return textContent_; }
    void setTextContent(const std::string& content) { textContent_ = content; }
    
    // Tree navigation
    DOMNode* getParentNode() const { return parent_; }
    std::vector<std::shared_ptr<DOMNode>>& getChildNodes() { return children_; }
    const std::vector<std::shared_ptr<DOMNode>>& getChildNodes() const { return children_; }
    
    // Tree manipulation
    void appendChild(std::shared_ptr<DOMNode> child);
    void insertBefore(std::shared_ptr<DOMNode> newChild, std::shared_ptr<DOMNode> referenceChild);
    void removeChild(std::shared_ptr<DOMNode> child);
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;
    
    // Utility methods
    virtual std::string toString() const;
    virtual std::string toHTML() const;

protected:
    DOMNodeType nodeType_;
    std::string nodeName_;
    std::string textContent_;
    DOMNode* parent_ = nullptr;
    std::vector<std::shared_ptr<DOMNode>> children_;
    std::unordered_map<std::string, std::string> attributes_;
    
    // Allow DOMElement to access children_ for getElementsByClassName
    friend class DOMElement;
};

//<---------DOM ELEMENT CLASS--------->
class DOMElement : public DOMNode {
public:
    DOMElement(const std::string& tagName);
    
    // Element properties
    std::string getTagName() const { return tagName_; }
    std::string getId() const;
    void setId(const std::string& id);
    std::string getClassName() const;
    void setClassName(const std::string& className);
    
    // Attribute management
    std::string getAttribute(const std::string& name) const;
    void setAttribute(const std::string& name, const std::string& value);
    void removeAttribute(const std::string& name);
    bool hasAttribute(const std::string& name) const;
    
    // CSS Styles
    void setStyle(const std::string& property, const std::string& value);
    std::string getStyle(const std::string& property) const;
    
    // Query methods
    std::shared_ptr<DOMElement> getElementById(const std::string& id);
    std::vector<std::shared_ptr<DOMElement>> getElementsByTagName(const std::string& tagName);
    std::vector<std::shared_ptr<DOMElement>> getElementsByClassName(const std::string& className);
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;
    
    // HTML generation
    std::string toHTML() const override;

private:
    std::string tagName_;
    std::unordered_map<std::string, std::string> styles_;
};

//<---------DOM DOCUMENT CLASS--------->
class DOMDocument : public DOMNode {
public:
    DOMDocument();
    
    // Document creation methods
    std::shared_ptr<DOMElement> createElement(const std::string& tagName);
    std::shared_ptr<DOMNode> createTextNode(const std::string& data);
    std::shared_ptr<DOMNode> createComment(const std::string& data);
    
    // Query methods
    std::shared_ptr<DOMElement> getElementById(const std::string& id);
    std::vector<std::shared_ptr<DOMElement>> getElementsByTagName(const std::string& tagName);
    std::vector<std::shared_ptr<DOMElement>> getElementsByClassName(const std::string& className);
    
    // Document properties
    std::shared_ptr<DOMElement> getDocumentElement() const { return documentElement_; }
    std::shared_ptr<DOMElement> getBody() const { return body_; }
    std::shared_ptr<DOMElement> getHead() const { return head_; }
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    std::shared_ptr<DOMElement> documentElement_;
    std::shared_ptr<DOMElement> head_;
    std::shared_ptr<DOMElement> body_;
};

//<---------DOM UTILITY FUNCTIONS--------->
std::shared_ptr<DOMDocument> createDocument();
std::string escapeHTML(const std::string& text);
std::string unescapeHTML(const std::string& html);

//<---------DOM GLOBAL OBJECT--------->
class DOMGlobal : public JSObject {
public:
    DOMGlobal(std::shared_ptr<DOMDocument> document);
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;
    
    std::shared_ptr<DOMDocument> getDocument() const { return document_; }

private:
    std::shared_ptr<DOMDocument> document_;
};

} // namespace Quanta

#endif // QUANTA_DOM_H
