#pragma once

#include <stdexcept>

namespace hitsc {

class UserError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace hitsc
