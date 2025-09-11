// Demonstration of IO<void> behavior and the new map_to method
#include "include/io_monad.hpp"
#include <iostream>

using namespace monad;

int main() {
    std::cout << "=== Understanding IO<void> ===\n\n";
    
    // 1. ORIGINAL DESIGN: IO<void> for side effects only
    std::cout << "1. Original map() - side effects only:\n";
    VoidIO::pure()
        .map([]() { 
            std::cout << "   Side effect: logging something\n"; 
            // return 42;  // This would be ignored!
        })
        .run([](VoidIO::IOResult result) {
            std::cout << "   Result: " << (result.is_ok() ? "Success (void)" : "Failed") << "\n\n";
        });
    
    // 2. USING then() - the traditional way to chain void -> value
    std::cout << "2. Using then() to chain void -> value:\n";
    VoidIO::pure()
        .then([]() { 
            std::cout << "   Transitioning from void to value...\n";
            return StringIO::pure("Hello from then()"); 
        })
        .run([](StringIO::IOResult result) {
            if (result.is_ok()) {
                std::cout << "   Result: " << result.value() << "\n\n";
            }
        });
    
    // 3. NEW METHOD: map_to() - direct void -> value transformation
    std::cout << "3. Using new map_to() method:\n";
    VoidIO::pure()
        .map_to([]() { 
            std::cout << "   Computing a value from void context...\n";
            return 42; 
        })
        .run([](IntIO::IOResult result) {
            if (result.is_ok()) {
                std::cout << "   Result: " << result.value() << "\n\n";
            }
        });
    
    // 4. CHAINING: void -> value -> transformation
    std::cout << "4. Chaining with map_to():\n";
    VoidIO::pure()
        .map_to([]() { 
            std::cout << "   Step 1: Generate initial value\n";
            return std::string("initial"); 
        })
        .map([](const std::string& s) { 
            std::cout << "   Step 2: Transform the value\n";
            return s + "_transformed"; 
        })
        .run([](StringIO::IOResult result) {
            if (result.is_ok()) {
                std::cout << "   Final result: " << result.value() << "\n\n";
            }
        });
    
    // 5. ERROR HANDLING: map_to preserves errors
    std::cout << "5. Error handling with map_to():\n";
    VoidIO::fail(Error{404, "Something went wrong"})
        .map_to([]() {
            std::cout << "   This won't be called!\n";
            return 99;
        })
        .run([](IntIO::IOResult result) {
            if (result.is_err()) {
                std::cout << "   Error preserved: [" << result.error().code 
                         << "] " << result.error().what << "\n\n";
            }
        });
    
    std::cout << "=== Summary ===\n";
    std::cout << "• map():    IO<void> -> IO<void> (side effects only)\n";
    std::cout << "• then():   IO<void> -> IO<U> (traditional monadic chain)\n";
    std::cout << "• map_to(): IO<void> -> IO<U> (direct value generation)\n";
    
    return 0;
}
