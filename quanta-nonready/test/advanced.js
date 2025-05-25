// Advanced test for Quanta JS Engine - Stage 1, 2 & 3
// Testing: Variables, expressions, arithmetic, built-in objects

// Variable declarations with different types
let number = 42;
var string = "Hello Quanta";
const boolean = true;
let result = 0;

// Arithmetic expressions
result = number + 10;
let multiplication = number * 2;
let division = number / 2;
let modulo = number % 5;

// Complex expressions
let complex = (number + 10) * 2 - 5;
let nested = number + (multiplication - division);

// String operations
var message = "Engine";
let greeting = "Quanta JS " + message;

// Boolean operations
let isPositive = number > 0;
let isValid = boolean && isPositive;

// Mathematical operations using built-in Math object
let absolute = Math.abs(-15);
let maximum = Math.max(10, 20, 30);
let power = Math.pow(2, 3);
let squareRoot = Math.sqrt(16);

// Console operations
console.log("Testing Quanta JS Engine");
console.log("Number:", number);
console.log("String:", string);
console.log("Boolean:", boolean);
console.log("Complex result:", complex);
console.log("Math operations - abs(-15):", absolute);
console.log("Math operations - max(10,20,30):", maximum);
console.log("Math operations - pow(2,3):", power);
console.log("Math operations - sqrt(16):", squareRoot);

// Assignment expressions
number = number + 5;
result = result * 2;

// Final calculations
let finalResult = number + result + complex;
console.log("Final result:", finalResult);
