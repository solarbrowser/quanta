//<---------QUANTA JS ENGINE - VIRTUAL DOM--------->
// Stage 4: DOM Integration & Frameworks - Virtual DOM System
// Purpose: Virtual DOM implementation for efficient DOM manipulation and framework support
// Max Lines: 5000 (Current: ~200)

#ifndef QUANTA_VDOM_H
#define QUANTA_VDOM_H

#include "dom.h"
#include "runtime_objects.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace Quanta {

//<---------VIRTUAL DOM NODE TYPES--------->
enum class VNodeType {
    ELEMENT,
    TEXT,
    COMPONENT,
    FRAGMENT
};

//<---------FORWARD DECLARATIONS--------->
class VNode;
class VElement;
class VText;
class VComponent;
class ComponentInstance;
class Renderer;

//<---------VIRTUAL NODE BASE CLASS--------->
class VNode {
public:
    VNode(VNodeType type) : type_(type) {}
    virtual ~VNode() = default;
    
    VNodeType getType() const { return type_; }
    virtual std::shared_ptr<VNode> clone() const = 0;
    virtual bool equals(const VNode& other) const = 0;
    virtual std::string toString() const = 0;

protected:
    VNodeType type_;
};

//<---------VIRTUAL ELEMENT NODE--------->
class VElement : public VNode {
public:
    VElement(const std::string& tagName);
    
    std::string getTagName() const { return tagName_; }
    
    // Props management
    void setProp(const std::string& name, const JSValue& value);
    JSValue getProp(const std::string& name) const;
    bool hasProp(const std::string& name) const;
    const std::unordered_map<std::string, JSValue>& getProps() const { return props_; }
    
    // Children management
    void addChild(std::shared_ptr<VNode> child);
    void setChildren(const std::vector<std::shared_ptr<VNode>>& children);
    const std::vector<std::shared_ptr<VNode>>& getChildren() const { return children_; }
    
    // Virtual methods
    std::shared_ptr<VNode> clone() const override;
    bool equals(const VNode& other) const override;
    std::string toString() const override;

private:
    std::string tagName_;
    std::unordered_map<std::string, JSValue> props_;
    std::vector<std::shared_ptr<VNode>> children_;
};

//<---------VIRTUAL TEXT NODE--------->
class VText : public VNode {
public:
    VText(const std::string& text);
    
    std::string getText() const { return text_; }
    void setText(const std::string& text) { text_ = text; }
    
    // Virtual methods
    std::shared_ptr<VNode> clone() const override;
    bool equals(const VNode& other) const override;
    std::string toString() const override;

private:
    std::string text_;
};

//<---------COMPONENT DEFINITION--------->
using ComponentFunction = std::function<std::shared_ptr<VNode>(const std::unordered_map<std::string, JSValue>&)>;

class ComponentDefinition {
public:
    ComponentDefinition(const std::string& name, ComponentFunction renderFn);
    
    std::string getName() const { return name_; }
    std::shared_ptr<VNode> render(const std::unordered_map<std::string, JSValue>& props) const;

private:
    std::string name_;
    ComponentFunction renderFunction_;
};

//<---------VIRTUAL COMPONENT NODE--------->
class VComponent : public VNode {
public:
    VComponent(const std::string& componentName);
    
    std::string getComponentName() const { return componentName_; }
    
    // Props management
    void setProp(const std::string& name, const JSValue& value);
    JSValue getProp(const std::string& name) const;
    const std::unordered_map<std::string, JSValue>& getProps() const { return props_; }
    
    // Virtual methods
    std::shared_ptr<VNode> clone() const override;
    bool equals(const VNode& other) const override;
    std::string toString() const override;

private:
    std::string componentName_;
    std::unordered_map<std::string, JSValue> props_;
};

//<---------DIFF RESULT--------->
enum class PatchType {
    CREATE,
    UPDATE_PROPS,
    UPDATE_TEXT,
    REPLACE,
    REMOVE,
    REORDER
};

struct Patch {
    PatchType type;
    std::shared_ptr<VNode> newNode = nullptr;
    std::shared_ptr<VNode> oldNode = nullptr;
    std::unordered_map<std::string, JSValue> propChanges;
    std::string newText;
    size_t index = 0;
};

//<---------VIRTUAL DOM DIFFER--------->
class VDOMDiffer {
public:
    static std::vector<Patch> diff(std::shared_ptr<VNode> oldNode, std::shared_ptr<VNode> newNode);
    
private:
    static std::vector<Patch> diffChildren(
        const std::vector<std::shared_ptr<VNode>>& oldChildren,
        const std::vector<std::shared_ptr<VNode>>& newChildren
    );
    
    static std::unordered_map<std::string, JSValue> diffProps(
        const std::unordered_map<std::string, JSValue>& oldProps,
        const std::unordered_map<std::string, JSValue>& newProps
    );
};

//<---------VIRTUAL DOM RENDERER--------->
class VDOMRenderer {
public:
    VDOMRenderer(std::shared_ptr<DOMDocument> document);
    
    // Rendering methods
    std::shared_ptr<DOMNode> render(std::shared_ptr<VNode> vnode);
    void patch(std::shared_ptr<DOMNode> domNode, const std::vector<Patch>& patches);
    
    // Update cycle
    void update(std::shared_ptr<VNode> oldVNode, std::shared_ptr<VNode> newVNode, std::shared_ptr<DOMNode> container);

private:
    std::shared_ptr<DOMDocument> document_;
    
    // Helper methods
    std::shared_ptr<DOMNode> createElement(const VElement& vElement);
    std::shared_ptr<DOMNode> createTextNode(const VText& vText);
    void updateElement(std::shared_ptr<DOMElement> element, const Patch& patch);
    void applyPatch(std::shared_ptr<DOMNode> domNode, const Patch& patch);
};

//<---------COMPONENT REGISTRY--------->
class ComponentRegistry {
public:
    static ComponentRegistry& getInstance();
    
    void registerComponent(const std::string& name, ComponentFunction renderFn);
    ComponentDefinition* getComponent(const std::string& name);
    bool hasComponent(const std::string& name) const;
    
    std::vector<std::string> getComponentNames() const;

private:
    ComponentRegistry() = default;
    std::unordered_map<std::string, std::unique_ptr<ComponentDefinition>> components_;
};

//<---------VIRTUAL DOM FACTORY FUNCTIONS--------->
std::shared_ptr<VElement> createElement(const std::string& tagName);
std::shared_ptr<VElement> createElement(const std::string& tagName, const std::unordered_map<std::string, JSValue>& props);
std::shared_ptr<VElement> createElement(const std::string& tagName, const std::unordered_map<std::string, JSValue>& props, const std::vector<std::shared_ptr<VNode>>& children);

std::shared_ptr<VText> createTextNode(const std::string& text);
std::shared_ptr<VComponent> createComponent(const std::string& componentName);
std::shared_ptr<VComponent> createComponent(const std::string& componentName, const std::unordered_map<std::string, JSValue>& props);

//<---------JSX-LIKE SYNTAX HELPERS--------->
class VNodeBuilder {
public:
    VNodeBuilder(const std::string& tagName);
    
    VNodeBuilder& prop(const std::string& name, const JSValue& value);
    VNodeBuilder& child(std::shared_ptr<VNode> child);
    VNodeBuilder& text(const std::string& text);
    VNodeBuilder& children(const std::vector<std::shared_ptr<VNode>>& children);
    
    std::shared_ptr<VElement> build();

private:
    std::shared_ptr<VElement> element_;
};

// Convenience macro for JSX-like syntax
#define VEL(tag) VNodeBuilder(tag)

//<---------VIRTUAL DOM GLOBAL--------->
class VDOMGlobal : public JSObject {
public:
    VDOMGlobal();
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;

private:
    ComponentRegistry& registry_;
};

} // namespace Quanta

#endif // QUANTA_VDOM_H
