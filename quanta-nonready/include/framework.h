//<---------QUANTA JS ENGINE - FRAMEWORK FOUNDATION--------->
// Stage 4: DOM Integration & Frameworks - Framework Support System
// Purpose: React-like component system, state management, and lifecycle hooks
// Max Lines: 5000 (Current: ~250)

#ifndef QUANTA_FRAMEWORK_H
#define QUANTA_FRAMEWORK_H

#include "vdom.h"
#include "dom.h"
#include "runtime_objects.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace Quanta {

//<---------FORWARD DECLARATIONS--------->
class Component;
class ComponentState;
class Hook;
class ReactiveSystem;
class FrameworkRuntime;

//<---------COMPONENT STATE MANAGEMENT--------->
class ComponentState {
public:
    ComponentState();
    
    // State operations
    void setState(const std::string& key, const JSValue& value);
    JSValue getState(const std::string& key) const;
    bool hasState(const std::string& key) const;
    
    // State subscription
    using StateChangeCallback = std::function<void(const std::string&, const JSValue&)>;
    void subscribe(StateChangeCallback callback);
    
    // Batch updates
    void batchUpdate(const std::function<void()>& updateFn);
    
    // State serialization
    std::unordered_map<std::string, JSValue> getAllState() const;
    void replaceState(const std::unordered_map<std::string, JSValue>& newState);

private:
    std::unordered_map<std::string, JSValue> state_;
    std::vector<StateChangeCallback> callbacks_;
    bool batchMode_ = false;
    std::vector<std::pair<std::string, JSValue>> pendingUpdates_;
    
    void notifyStateChange(const std::string& key, const JSValue& value);
    void flushUpdates();
};

//<---------COMPONENT LIFECYCLE--------->
enum class ComponentLifecycle {
    WILL_MOUNT,
    DID_MOUNT,
    WILL_UPDATE,
    DID_UPDATE,
    WILL_UNMOUNT
};

//<---------HOOK SYSTEM--------->
class Hook {
public:
    virtual ~Hook() = default;
    virtual void initialize() {}
    virtual void cleanup() {}
    virtual JSValue execute(const std::vector<JSValue>& args) = 0;
};

class UseStateHook : public Hook {
public:
    UseStateHook(ComponentState& state, const std::string& key, const JSValue& initialValue);
    JSValue execute(const std::vector<JSValue>& args) override;

private:
    ComponentState& state_;
    std::string key_;
    JSValue initialValue_;
};

class UseEffectHook : public Hook {
public:
    using EffectFunction = std::function<std::function<void()>()>; // Effect function that returns cleanup
    using DependencyArray = std::vector<JSValue>;
    
    UseEffectHook(EffectFunction effect, const DependencyArray& deps = {});
    
    void initialize() override;
    void cleanup() override;
    JSValue execute(const std::vector<JSValue>& args) override;

private:
    EffectFunction effect_;
    DependencyArray dependencies_;
    DependencyArray lastDependencies_;
    std::function<void()> cleanup_;
    bool hasRun_ = false;
    
    bool dependenciesChanged() const;
};

//<---------COMPONENT BASE CLASS--------->
class Component {
public:
    Component(const std::string& name);
    virtual ~Component() = default;
    
    // Component identification
    std::string getName() const { return name_; }
    std::string getId() const { return id_; }
    
    // Props management
    void setProps(const std::unordered_map<std::string, JSValue>& props);
    JSValue getProp(const std::string& name) const;
    bool hasProp(const std::string& name) const;
    
    // State management
    ComponentState& getState() { return state_; }
    const ComponentState& getState() const { return state_; }
    
    // Lifecycle methods (to be overridden)
    virtual void componentWillMount() {}
    virtual void componentDidMount() {}
    virtual void componentWillUpdate(const std::unordered_map<std::string, JSValue>& nextProps) {}
    virtual void componentDidUpdate(const std::unordered_map<std::string, JSValue>& prevProps) {}
    virtual void componentWillUnmount() {}
    
    // Render method (must be overridden)
    virtual std::shared_ptr<VNode> render() = 0;
    
    // Component lifecycle
    void mount();
    void update(const std::unordered_map<std::string, JSValue>& newProps);
    void unmount();
    
    // Hook management
    template<typename T, typename... Args>
    T* useHook(Args&&... args) {
        auto hook = std::make_unique<T>(std::forward<Args>(args)...);
        T* hookPtr = hook.get();
        hooks_.push_back(std::move(hook));
        return hookPtr;
    }
    
    // Force re-render
    void forceUpdate();

protected:
    std::string name_;
    std::string id_;
    std::unordered_map<std::string, JSValue> props_;
    ComponentState state_;
    std::vector<std::unique_ptr<Hook>> hooks_;
    bool mounted_ = false;
    
    // Utility methods for subclasses
    void setState(const std::string& key, const JSValue& value);
    JSValue getStateValue(const std::string& key) const;

private:
    static size_t nextId_;
    
    void initializeHooks();
    void cleanupHooks();
    void callLifecycleMethod(ComponentLifecycle lifecycle);
};

//<---------FUNCTIONAL COMPONENT--------->
using FunctionalComponent = std::function<std::shared_ptr<VNode>(const std::unordered_map<std::string, JSValue>&)>;

class FunctionalComponentWrapper : public Component {
public:
    FunctionalComponentWrapper(const std::string& name, FunctionalComponent renderFn);
    
    std::shared_ptr<VNode> render() override;

private:
    FunctionalComponent renderFunction_;
};

//<---------COMPONENT FACTORY--------->
class ComponentFactory {
public:
    static ComponentFactory& getInstance();
    
    // Register components
    void registerComponent(const std::string& name, std::function<std::unique_ptr<Component>()> factory);
    void registerFunctionalComponent(const std::string& name, FunctionalComponent renderFn);
    
    // Create components
    std::unique_ptr<Component> createComponent(const std::string& name);
    bool hasComponent(const std::string& name) const;
    
    std::vector<std::string> getComponentNames() const;

private:
    ComponentFactory() = default;
    std::unordered_map<std::string, std::function<std::unique_ptr<Component>()>> factories_;
    std::unordered_map<std::string, FunctionalComponent> functionalComponents_;
};

//<---------REACTIVE SYSTEM--------->
class ReactiveSystem {
public:
    static ReactiveSystem& getInstance();
    
    // Reactive values
    using ReactiveCallback = std::function<void(const JSValue&)>;
    
    void createReactive(const std::string& name, const JSValue& initialValue);
    void setReactive(const std::string& name, const JSValue& value);
    JSValue getReactive(const std::string& name) const;
    
    // Subscriptions
    void subscribe(const std::string& name, ReactiveCallback callback);
    void unsubscribe(const std::string& name, ReactiveCallback callback);
    
    // Computed values
    using ComputedFunction = std::function<JSValue()>;
    void createComputed(const std::string& name, ComputedFunction computeFn);
    
    // Watch for changes
    void watch(const std::string& name, ReactiveCallback callback);

private:
    ReactiveSystem() = default;
    
    struct ReactiveValue {
        JSValue value;
        std::vector<ReactiveCallback> callbacks;
    };
    
    struct ComputedValue {
        ComputedFunction computeFn;
        JSValue cachedValue;
        bool dirty = true;
        std::vector<ReactiveCallback> callbacks;
    };
    
    std::unordered_map<std::string, ReactiveValue> reactives_;
    std::unordered_map<std::string, ComputedValue> computed_;
    
    void notifySubscribers(const std::string& name, const JSValue& value);
    void markComputedDirty(const std::string& name);
};

//<---------COMPONENT TREE MANAGER--------->
class ComponentTree {
public:
    struct TreeNode {
        std::unique_ptr<Component> component;
        std::shared_ptr<VNode> vnode;
        std::shared_ptr<DOMNode> domNode;
        std::vector<std::unique_ptr<TreeNode>> children;
        TreeNode* parent = nullptr;
    };
    
    ComponentTree();
    
    // Tree operations
    TreeNode* mountComponent(const std::string& componentName, 
                           const std::unordered_map<std::string, JSValue>& props,
                           TreeNode* parent = nullptr);
    
    void unmountComponent(TreeNode* node);
    void updateComponent(TreeNode* node, const std::unordered_map<std::string, JSValue>& newProps);
    
    // Tree navigation
    TreeNode* getRoot() const { return root_.get(); }
    TreeNode* findComponent(const std::string& id);
    
    // Rendering
    void renderTree(std::shared_ptr<DOMElement> container);

private:
    std::unique_ptr<TreeNode> root_;
    VDOMRenderer renderer_;
    
    void renderNode(TreeNode* node, std::shared_ptr<DOMElement> container);
    TreeNode* findComponentRecursive(TreeNode* node, const std::string& id);
};

//<---------FRAMEWORK RUNTIME--------->
class FrameworkRuntime {
public:
    FrameworkRuntime(std::shared_ptr<DOMDocument> document);
    
    // Application lifecycle
    void initialize();
    void shutdown();
    
    // Component management
    ComponentTree& getComponentTree() { return componentTree_; }
    ComponentFactory& getComponentFactory() { return ComponentFactory::getInstance(); }
    ReactiveSystem& getReactiveSystem() { return ReactiveSystem::getInstance(); }
    
    // Rendering
    void render(const std::string& componentName, 
               const std::unordered_map<std::string, JSValue>& props,
               std::shared_ptr<DOMElement> container);
    
    void scheduleUpdate();
    void forceUpdate();
    
    // Event handling
    void handleEvent(const Event& event);

private:
    std::shared_ptr<DOMDocument> document_;
    ComponentTree componentTree_;
    bool updateScheduled_ = false;
    
    void performUpdate();
};

//<---------FRAMEWORK GLOBAL OBJECT--------->
class FrameworkGlobal : public JSObject {
public:
    FrameworkGlobal(std::shared_ptr<FrameworkRuntime> runtime);
      // JS Object interface
    JSValue getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const JSValue& value) override;
    
    std::shared_ptr<FrameworkRuntime> getRuntime() const { return runtime_; }

private:
    std::shared_ptr<FrameworkRuntime> runtime_;
};

//<---------UTILITY MACROS--------->
#define COMPONENT(name) \
    class name : public Component { \
    public: \
        name() : Component(#name) {} \
        std::shared_ptr<VNode> render() override; \
    }; \
    std::shared_ptr<VNode> name::render()

#define REGISTER_COMPONENT(name) \
    ComponentFactory::getInstance().registerComponent(#name, []() { \
        return std::make_unique<name>(); \
    })

#define FUNCTIONAL_COMPONENT(name, props) \
    auto name = [](const std::unordered_map<std::string, JSValue>& props) -> std::shared_ptr<VNode>

} // namespace Quanta

#endif // QUANTA_FRAMEWORK_H
