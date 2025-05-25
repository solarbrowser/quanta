//<---------QUANTA JS ENGINE - FRAMEWORK IMPLEMENTATION--------->
// Stage 4: DOM Integration & Frameworks - Framework Support System
// Purpose: React-like component system, state management, and lifecycle hooks
// Max Lines: 5000 (Current: ~500)

#include "../include/framework.h"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace Quanta {

//<---------COMPONENT STATE IMPLEMENTATION--------->
ComponentState::ComponentState() {
}

void ComponentState::setState(const std::string& key, const JSValue& value) {
    if (batchMode_) {
        pendingUpdates_.push_back({key, value});
    } else {
        state_[key] = value;
        notifyStateChange(key, value);
    }
}

JSValue ComponentState::getState(const std::string& key) const {
    auto it = state_.find(key);
    return (it != state_.end()) ? it->second : JSValue();
}

bool ComponentState::hasState(const std::string& key) const {
    return state_.find(key) != state_.end();
}

void ComponentState::subscribe(StateChangeCallback callback) {
    callbacks_.push_back(callback);
}

void ComponentState::batchUpdate(const std::function<void()>& updateFn) {
    batchMode_ = true;
    pendingUpdates_.clear();
    
    updateFn();
    
    batchMode_ = false;
    flushUpdates();
}

std::unordered_map<std::string, JSValue> ComponentState::getAllState() const {
    return state_;
}

void ComponentState::replaceState(const std::unordered_map<std::string, JSValue>& newState) {
    state_ = newState;
    
    // Notify all state changes
    for (const auto& stateItem : state_) {
        notifyStateChange(stateItem.first, stateItem.second);
    }
}

void ComponentState::notifyStateChange(const std::string& key, const JSValue& value) {
    for (const auto& callback : callbacks_) {
        callback(key, value);
    }
}

void ComponentState::flushUpdates() {
    for (const auto& update : pendingUpdates_) {
        state_[update.first] = update.second;
        notifyStateChange(update.first, update.second);
    }
    pendingUpdates_.clear();
}

//<---------USE STATE HOOK IMPLEMENTATION--------->
UseStateHook::UseStateHook(ComponentState& state, const std::string& key, const JSValue& initialValue)
    : state_(state), key_(key), initialValue_(initialValue) {
    if (!state_.hasState(key_)) {
        state_.setState(key_, initialValue_);
    }
}

JSValue UseStateHook::execute(const std::vector<JSValue>& args) {
    if (args.empty()) {
        return state_.getState(key_);
    } else {
        state_.setState(key_, args[0]);
        return JSValue();
    }
}

//<---------USE EFFECT HOOK IMPLEMENTATION--------->
UseEffectHook::UseEffectHook(EffectFunction effect, const DependencyArray& deps)
    : effect_(effect), dependencies_(deps) {
}

void UseEffectHook::initialize() {
    if (!hasRun_ || dependenciesChanged()) {
        cleanup();
        cleanup_ = effect_();
        lastDependencies_ = dependencies_;
        hasRun_ = true;
    }
}

void UseEffectHook::cleanup() {
    if (cleanup_) {
        cleanup_();
        cleanup_ = nullptr;
    }
}

JSValue UseEffectHook::execute(const std::vector<JSValue>& args) {
    // Update dependencies if provided
    if (!args.empty()) {
        dependencies_.clear();
        for (const auto& arg : args) {
            dependencies_.push_back(arg);
        }
    }
    
    if (dependenciesChanged()) {
        initialize();
    }
    
    return JSValue();
}

bool UseEffectHook::dependenciesChanged() const {
    if (dependencies_.size() != lastDependencies_.size()) {
        return true;
    }
    
    for (size_t i = 0; i < dependencies_.size(); ++i) {
        if (dependencies_[i] != lastDependencies_[i]) {
            return true;
        }
    }
    
    return false;
}

//<---------COMPONENT IMPLEMENTATION--------->
size_t Component::nextId_ = 0;

Component::Component(const std::string& name) : name_(name) {
    id_ = name + "_" + std::to_string(nextId_++);
    
    // Subscribe to state changes for automatic re-rendering
    state_.subscribe([this](const std::string& /*key*/, const JSValue& /*value*/) {
        if (mounted_) {
            forceUpdate();
        }
    });
}

void Component::setProps(const std::unordered_map<std::string, JSValue>& props) {
    auto oldProps = props_;
    props_ = props;
    
    if (mounted_) {
        componentWillUpdate(props);
        componentDidUpdate(oldProps);
    }
}

JSValue Component::getProp(const std::string& name) const {
    auto it = props_.find(name);
    return (it != props_.end()) ? it->second : JSValue();
}

bool Component::hasProp(const std::string& name) const {
    return props_.find(name) != props_.end();
}

void Component::mount() {
    if (!mounted_) {
        callLifecycleMethod(ComponentLifecycle::WILL_MOUNT);
        initializeHooks();
        mounted_ = true;
        callLifecycleMethod(ComponentLifecycle::DID_MOUNT);
    }
}

void Component::update(const std::unordered_map<std::string, JSValue>& newProps) {
    if (mounted_) {
        setProps(newProps);
    }
}

void Component::unmount() {
    if (mounted_) {
        callLifecycleMethod(ComponentLifecycle::WILL_UNMOUNT);
        cleanupHooks();
        mounted_ = false;
    }
}

void Component::forceUpdate() {
    // Trigger re-render
    // In a real implementation, this would notify the framework runtime
    std::cout << "[Component " << id_ << "] Force update triggered\n";
}

void Component::setState(const std::string& key, const JSValue& value) {
    state_.setState(key, value);
}

JSValue Component::getStateValue(const std::string& key) const {
    return state_.getState(key);
}

void Component::initializeHooks() {
    for (auto& hook : hooks_) {
        hook->initialize();
    }
}

void Component::cleanupHooks() {
    for (auto& hook : hooks_) {
        hook->cleanup();
    }
}

void Component::callLifecycleMethod(ComponentLifecycle lifecycle) {
    switch (lifecycle) {
        case ComponentLifecycle::WILL_MOUNT:
            componentWillMount();
            break;
        case ComponentLifecycle::DID_MOUNT:
            componentDidMount();
            break;
        case ComponentLifecycle::WILL_UPDATE:
            // componentWillUpdate is called with props in update()
            break;
        case ComponentLifecycle::DID_UPDATE:
            // componentDidUpdate is called with props in update()
            break;
        case ComponentLifecycle::WILL_UNMOUNT:
            componentWillUnmount();
            break;
    }
}

//<---------FUNCTIONAL COMPONENT WRAPPER--------->
FunctionalComponentWrapper::FunctionalComponentWrapper(const std::string& name, FunctionalComponent renderFn)
    : Component(name), renderFunction_(renderFn) {
}

std::shared_ptr<VNode> FunctionalComponentWrapper::render() {
    return renderFunction_(props_);
}

//<---------COMPONENT FACTORY IMPLEMENTATION--------->
ComponentFactory& ComponentFactory::getInstance() {
    static ComponentFactory instance;
    return instance;
}

void ComponentFactory::registerComponent(const std::string& name, std::function<std::unique_ptr<Component>()> factory) {
    factories_[name] = factory;
}

void ComponentFactory::registerFunctionalComponent(const std::string& name, FunctionalComponent renderFn) {
    functionalComponents_[name] = renderFn;
}

std::unique_ptr<Component> ComponentFactory::createComponent(const std::string& name) {
    // Try regular components first
    auto it = factories_.find(name);
    if (it != factories_.end()) {
        return it->second();
    }
    
    // Try functional components
    auto funcIt = functionalComponents_.find(name);
    if (funcIt != functionalComponents_.end()) {
        return std::make_unique<FunctionalComponentWrapper>(name, funcIt->second);
    }
    
    return nullptr;
}

bool ComponentFactory::hasComponent(const std::string& name) const {
    return factories_.find(name) != factories_.end() || 
           functionalComponents_.find(name) != functionalComponents_.end();
}

std::vector<std::string> ComponentFactory::getComponentNames() const {
    std::vector<std::string> names;
    
    for (const auto& factory : factories_) {
        names.push_back(factory.first);
    }
    
    for (const auto& func : functionalComponents_) {
        names.push_back(func.first);
    }
    
    return names;
}

//<---------REACTIVE SYSTEM IMPLEMENTATION--------->
ReactiveSystem& ReactiveSystem::getInstance() {
    static ReactiveSystem instance;
    return instance;
}

void ReactiveSystem::createReactive(const std::string& name, const JSValue& initialValue) {
    reactives_[name] = {initialValue, {}};
}

void ReactiveSystem::setReactive(const std::string& name, const JSValue& value) {
    auto it = reactives_.find(name);
    if (it != reactives_.end()) {
        it->second.value = value;
        notifySubscribers(name, value);
    }
}

JSValue ReactiveSystem::getReactive(const std::string& name) const {
    auto it = reactives_.find(name);
    return (it != reactives_.end()) ? it->second.value : JSValue();
}

void ReactiveSystem::subscribe(const std::string& name, ReactiveCallback callback) {
    reactives_[name].callbacks.push_back(callback);
}

void ReactiveSystem::unsubscribe(const std::string& name, ReactiveCallback callback) {
    auto it = reactives_.find(name);
    if (it != reactives_.end()) {
        // In a real implementation, we'd need a way to identify and remove specific callbacks
        // This is simplified
        it->second.callbacks.clear();
    }
}

void ReactiveSystem::createComputed(const std::string& name, ComputedFunction computeFn) {
    computed_[name] = {computeFn, JSValue(), true, {}};
}

void ReactiveSystem::watch(const std::string& name, ReactiveCallback callback) {
    subscribe(name, callback);
}

void ReactiveSystem::notifySubscribers(const std::string& name, const JSValue& value) {
    auto it = reactives_.find(name);
    if (it != reactives_.end()) {
        for (const auto& callback : it->second.callbacks) {
            callback(value);
        }
    }
    
    // Mark dependent computed values as dirty
    markComputedDirty(name);
}

void ReactiveSystem::markComputedDirty(const std::string& /*name*/) {
    // In a real implementation, we'd track dependencies and mark computed values as dirty
    for (auto& computed : computed_) {
        computed.second.dirty = true;
    }
}

//<---------COMPONENT TREE IMPLEMENTATION--------->
ComponentTree::ComponentTree() : renderer_(nullptr) {
    // Renderer needs to be initialized with a document
}

ComponentTree::TreeNode* ComponentTree::mountComponent(const std::string& componentName, 
                                                      const std::unordered_map<std::string, JSValue>& props,
                                                      TreeNode* parent) {
    auto component = ComponentFactory::getInstance().createComponent(componentName);
    if (!component) {
        return nullptr;
    }
    
    component->setProps(props);
    component->mount();
    
    auto node = std::make_unique<TreeNode>();
    node->component = std::move(component);
    node->vnode = node->component->render();
    node->parent = parent;
    
    TreeNode* nodePtr = node.get();
    
    if (parent) {
        parent->children.push_back(std::move(node));
    } else {
        root_ = std::move(node);
    }
    
    return nodePtr;
}

void ComponentTree::unmountComponent(TreeNode* node) {
    if (!node) return;
    
    // Unmount all children first
    for (auto& child : node->children) {
        unmountComponent(child.get());
    }
    
    // Unmount this component
    if (node->component) {
        node->component->unmount();
    }
    
    // Remove from parent
    if (node->parent) {
        auto& siblings = node->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [node](const std::unique_ptr<TreeNode>& child) {
                    return child.get() == node;
                }),
            siblings.end()
        );
    } else {
        root_.reset();
    }
}

void ComponentTree::updateComponent(TreeNode* node, const std::unordered_map<std::string, JSValue>& newProps) {
    if (!node || !node->component) return;
    
    node->component->update(newProps);
    node->vnode = node->component->render();
    
    // In a real implementation, we'd diff the old and new vnodes and update the DOM
}

ComponentTree::TreeNode* ComponentTree::findComponent(const std::string& id) {
    return findComponentRecursive(root_.get(), id);
}

void ComponentTree::renderTree(std::shared_ptr<DOMElement> container) {
    if (root_) {
        renderNode(root_.get(), container);
    }
}

void ComponentTree::renderNode(TreeNode* node, std::shared_ptr<DOMElement> container) {
    if (!node || !node->vnode) return;
    
    // Render the virtual node to DOM
    if (renderer_.render(node->vnode)) {
        // In a real implementation, we'd append the rendered DOM to the container
    }
    
    // Render children
    for (auto& child : node->children) {
        renderNode(child.get(), container);
    }
}

ComponentTree::TreeNode* ComponentTree::findComponentRecursive(TreeNode* node, const std::string& id) {
    if (!node) return nullptr;
    
    if (node->component && node->component->getId() == id) {
        return node;
    }
    
    for (auto& child : node->children) {
        if (auto found = findComponentRecursive(child.get(), id)) {
            return found;
        }
    }
    
    return nullptr;
}

//<---------FRAMEWORK RUNTIME IMPLEMENTATION--------->
FrameworkRuntime::FrameworkRuntime(std::shared_ptr<DOMDocument> document) 
    : document_(document), componentTree_() {
}

void FrameworkRuntime::initialize() {
    std::cout << "[FrameworkRuntime] Initializing...\n";
}

void FrameworkRuntime::shutdown() {
    std::cout << "[FrameworkRuntime] Shutting down...\n";
    
    // Unmount all components
    if (auto root = componentTree_.getRoot()) {
        componentTree_.unmountComponent(root);
    }
}

void FrameworkRuntime::render(const std::string& componentName, 
                             const std::unordered_map<std::string, JSValue>& props,
                             std::shared_ptr<DOMElement> container) {
    
    auto rootComponent = componentTree_.mountComponent(componentName, props);
    if (rootComponent) {
        componentTree_.renderTree(container);
        std::cout << "[FrameworkRuntime] Rendered component: " << componentName << "\n";
    } else {
        std::cout << "[FrameworkRuntime] Failed to create component: " << componentName << "\n";
    }
}

void FrameworkRuntime::scheduleUpdate() {
    if (!updateScheduled_) {
        updateScheduled_ = true;
        // In a real implementation, we'd schedule this for the next frame
        performUpdate();
    }
}

void FrameworkRuntime::forceUpdate() {
    performUpdate();
}

void FrameworkRuntime::handleEvent(const Event& event) {
    std::cout << "[FrameworkRuntime] Handling event: " << event.getType() << "\n";
    // Event handling logic would go here
}

void FrameworkRuntime::performUpdate() {
    updateScheduled_ = false;
    
    // Re-render the component tree
    if (document_->getBody()) {
        componentTree_.renderTree(document_->getBody());
    }
    
    std::cout << "[FrameworkRuntime] Update performed\n";
}

//<---------FRAMEWORK GLOBAL IMPLEMENTATION--------->
FrameworkGlobal::FrameworkGlobal(std::shared_ptr<FrameworkRuntime> runtime) : runtime_(runtime) {
}

JSValue FrameworkGlobal::getProperty(const std::string& name) {
    if (name == "createComponent") {
        return JSValue(std::string("function"));
    }
    if (name == "useState") {
        return JSValue(std::string("function"));
    }
    if (name == "useEffect") {
        return JSValue(std::string("function"));
    }
    if (name == "render") {
        return JSValue(std::string("function"));
    }
    
    return JSObject::getProperty(name);
}

void FrameworkGlobal::setProperty(const std::string& name, const JSValue& value) {
    JSObject::setProperty(name, value);
}

} // namespace Quanta
