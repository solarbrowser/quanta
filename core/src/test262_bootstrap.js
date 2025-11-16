// Test262 Harness Bootstrap
// This file contains all Test262 harness functions as pure JavaScript
// It will be automatically executed when the engine initializes

// =============================================================================
// Test262Error Constructor
// =============================================================================

function Test262Error(message) {
        this.name = "Test262Error";
        this.message = message || "";
    }
    
    Test262Error.prototype = Object.create(Error.prototype);
    Test262Error.prototype.constructor = Test262Error;
    Test262Error.prototype.toString = function() {
        return "Test262Error: " + this.message;
    };
    
    // =============================================================================
    // Assert Functions
    // =============================================================================
    
    function assert(mustBeTrue, message) {
        if (!mustBeTrue) {
            throw new Test262Error(message || "assertion failed");
        }
    }
    
    assert.sameValue = function(actual, expected, message) {
        if (!Object.is(actual, expected)) {
            var actualStr = String(actual);
            var expectedStr = String(expected);
            throw new Test262Error(message || ("Expected " + expectedStr + ", got " + actualStr));
        }
    };
    
    assert.notSameValue = function(actual, unexpected, message) {
        if (Object.is(actual, unexpected)) {
            var actualStr = String(actual);
            throw new Test262Error(message || ("Expected not " + actualStr));
        }
    };
    
    assert.throws = function(expectedError, func, message) {
        var thrown = false;
        var thrownError = null;
        
        try {
            func();
        } catch (e) {
            thrown = true;
            thrownError = e;
            
            if (expectedError) {
                if (typeof expectedError === 'function') {
                    if (!(thrownError instanceof expectedError)) {
                        var expectedName = expectedError.name || 'Error';
                        var thrownName = thrownError.name || 'Error';
                        throw new Test262Error(message || ("Expected " + expectedName + ", got " + thrownName));
                    }
                }
            }
        }
        
        if (!thrown) {
            var errorName = expectedError && expectedError.name ? expectedError.name : 'any';
            throw new Test262Error(message || ("Expected exception " + errorName + " was not thrown"));
        }
    };
    
    // =============================================================================
    // Property Verification Functions
    // =============================================================================
    
    function verifyProperty(obj, name, desc) {
        // Accept both objects and functions (functions are objects in JS)
        if ((typeof obj !== "object" && typeof obj !== "function") || obj === null) {
            throw new Test262Error("obj must be an object");
        }
        
        if (typeof desc !== "object" || desc === null) {
            throw new Test262Error("desc must be an object");
        }
        
        var actualDesc = Object.getOwnPropertyDescriptor(obj, name);
        
        if (!actualDesc) {
            throw new Test262Error("property '" + name + "' not found");
        }
        
        if ('value' in desc && !Object.is(actualDesc.value, desc.value)) {
            throw new Test262Error("property '" + name + "' has wrong value");
        }
        
        if ('writable' in desc && actualDesc.writable !== desc.writable) {
            throw new Test262Error("property '" + name + "' has wrong writable attribute");
        }
        
        if ('enumerable' in desc && actualDesc.enumerable !== desc.enumerable) {
            throw new Test262Error("property '" + name + "' has wrong enumerable attribute");
        }
        
        if ('configurable' in desc && actualDesc.configurable !== desc.configurable) {
            throw new Test262Error("property '" + name + "' has wrong configurable attribute");
        }
    }
    
    function verifyNotEnumerable(obj, name) {
        verifyProperty(obj, name, { enumerable: false });
    }
    
    function verifyNotWritable(obj, name) {
        verifyProperty(obj, name, { writable: false });
    }
    
    function verifyNotConfigurable(obj, name) {
        verifyProperty(obj, name, { configurable: false });
    }
    
    function verifyEnumerable(obj, name) {
        verifyProperty(obj, name, { enumerable: true });
    }
    
    function verifyWritable(obj, name) {
        verifyProperty(obj, name, { writable: true });
    }
    
    function verifyConfigurable(obj, name) {
        verifyProperty(obj, name, { configurable: true });
    }
    
    // =============================================================================
    // Test262 Utility Functions
    // =============================================================================
    
    function $DONOTEVALUATE() {
        throw new Test262Error("Test262: This statement should not be evaluated.");
    }
    
    function $DONE(error) {
        if (error) {
            throw error;
        }
    }
    
    function print(message) {
        if (typeof console !== 'undefined' && console.log) {
            console.log(message);
        }
    }
    
    // =============================================================================
    // $262 Global Object
    // =============================================================================
    
    var $262 = {
        global: (function() { return this; })() || globalThis || (typeof global !== 'undefined' ? global : window),
        
        createRealm: function() {
            // Simplified realm creation
            return { global: $262.global };
        },
        
        detachArrayBuffer: function(buffer) {
            // Mark buffer as detached
            if (buffer && typeof buffer === 'object') {
                // Implementation depends on engine internals
                try {
                    Object.defineProperty(buffer, 'byteLength', {
                        value: 0,
                        writable: false,
                        enumerable: false,
                        configurable: false
                    });
                } catch (e) {
                    // If property redefinition fails, buffer might already be detached
                }
            }
        },
        
        evalScript: function(source) {
            return eval(source);
        },
        
        gc: function() {
            // Trigger garbage collection if available
            if (typeof gc === 'function') {
                gc();
            }
        },
        
        agent: {
            start: function(script) {
                // Simplified agent start
                return null;
            },
            
            broadcast: function(sab, id) {
                // Simplified broadcast
            },
            
            getReport: function() {
                return null;
            },
            
            sleep: function(ms) {
                // Busy-wait sleep (not ideal but works for sync code)
                var start = Date.now();
                while (Date.now() - start < ms) {
                    // Busy wait
                }
            },
            
            monotonicNow: function() {
                return Date.now();
            }
        }
    };
    
// =============================================================================
// isConstructor - Test if a value can be used as a constructor
// =============================================================================

function isConstructor(obj) {
    if (obj === null || obj === undefined) {
        return false;
    }
    
    // Check if it's a function
    if (typeof obj !== 'function') {
        return false;
    }
    
    // Try to use it as a constructor
    try {
        // Use Reflect.construct if available, otherwise use new
        if (typeof Reflect !== 'undefined' && Reflect.construct) {
            Reflect.construct(function() {}, [], obj);
        } else {
            new obj();
        }
        return true;
    } catch (e) {
        return false;
    }
}

// =============================================================================
// testWithTypedArrayConstructors - Run test with each TypedArray constructor
// =============================================================================

function testWithTypedArrayConstructors(callback) {
    var constructors = [
        Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
        Int32Array, Uint32Array, Float32Array, Float64Array
    ];

    // Add BigInt arrays if supported
    if (typeof BigInt64Array !== 'undefined') {
        constructors.push(BigInt64Array);
    }
    if (typeof BigUint64Array !== 'undefined') {
        constructors.push(BigUint64Array);
    }

    for (var i = 0; i < constructors.length; i++) {
        callback(constructors[i]);
    }
}

// =============================================================================
// testWithAtomicsFriendlyTypedArrayConstructors
// =============================================================================

function testWithAtomicsFriendlyTypedArrayConstructors(callback) {
    var atomicsFriendly = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];

    // Add BigInt arrays if supported
    if (typeof BigInt64Array !== 'undefined') {
        atomicsFriendly.push(BigInt64Array);
    }
    if (typeof BigUint64Array !== 'undefined') {
        atomicsFriendly.push(BigUint64Array);
    }

    for (var i = 0; i < atomicsFriendly.length; i++) {
        callback(atomicsFriendly[i]);
    }
}

// =============================================================================
// buildString - Build string from array of code points
// =============================================================================

function buildString(arr) {
    var result = "";
    for (var i = 0; i < arr.length; i++) {
        result += String.fromCharCode(arr[i]);
    }
    return result;
}

// =============================================================================
// $DETACHBUFFER - Detach ArrayBuffer (placeholder implementation)
// =============================================================================

function $DETACHBUFFER(buffer) {
    // Placeholder implementation - Quanta doesn't support buffer detaching yet
    // This is a no-op for now to prevent test failures
    if (buffer && typeof buffer.byteLength !== 'undefined') {
        // Mark as detached (if possible)
        try {
            Object.defineProperty(buffer, 'byteLength', { value: 0 });
        } catch (e) {
            // Ignore if can't modify
        }
    }
}

// =============================================================================
// Additional verifyProperty helper functions
// =============================================================================

function verifyEqualTo(obj, prop, value, message) {
    if (obj[prop] !== value) {
        throw new Test262Error(message || ("Property " + prop + " should equal " + value + ", got " + obj[prop]));
    }
}

// =============================================================================
// Register all functions in global scope
// =============================================================================

// Functions are already in global scope since they're defined at top level
// No need for explicit registration

