#include <iostream>
#include "include/result_monad.hpp"

using namespace monad;

// Example functions that return void results
MyVoidResult validate_input() {
    std::cout << "Validating input...\n";
    return MyVoidResult::Ok();
}

MyResult<int> process_data() {
    std::cout << "Processing data...\n";
    return MyResult<int>::Ok(42);
}

MyResult<std::string> format_result(int value) {
    std::cout << "Formatting result: " << value << "\n";
    return MyResult<std::string>::Ok("Result: " + std::to_string(value));
}

int main() {
    // Chain void -> int -> string using and_then
    auto result = validate_input()
        .and_then([]() { return process_data(); })
        .and_then([](int value) { return format_result(value); });
    
    if (result.is_ok()) {
        std::cout << "Success: " << result.value() << std::endl;
    } else {
        std::cout << "Error: " << result.error().what << std::endl;
    }
    
    // Test error case
    auto error_result = MyVoidResult::Err(Error{404, "Not found"})
        .and_then([]() { return process_data(); })
        .and_then([](int value) { return format_result(value); });
    
    if (error_result.is_err()) {
        std::cout << "Expected error: " << error_result.error().what << std::endl;
    }
    
    return 0;
}
