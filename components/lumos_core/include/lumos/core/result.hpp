#pragma once

#include <string>
#include <utility>
#include <variant>

namespace lumos {

enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    NotInitialized,
    Busy,
    Timeout,
    IoError,
    OutOfMemory,
    NetworkError,
    ParseError,
    NotSupported,
    Internal,
};

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message;

    static Error make(ErrorCode code, std::string message) {
        return Error{code, std::move(message)};
    }
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    static Result ok(T value) { return Result(std::move(value)); }
    static Result fail(Error error) { return Result(std::move(error)); }
    static Result fail(ErrorCode code, std::string message) {
        return fail(Error::make(code, std::move(message)));
    }

    bool is_ok() const { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const { return is_ok(); }

    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }

    Error& error() { return std::get<Error>(storage_); }
    const Error& error() const { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() : ok_(true) {}
    Result(Error error) : ok_(false), error_(std::move(error)) {}

    static Result ok() { return Result(); }
    static Result fail(Error error) { return Result(std::move(error)); }
    static Result fail(ErrorCode code, std::string message) {
        return fail(Error::make(code, std::move(message)));
    }

    bool is_ok() const { return ok_; }
    explicit operator bool() const { return is_ok(); }

    Error& error() { return error_; }
    const Error& error() const { return error_; }

private:
    bool ok_{true};
    Error error_{};
};

} // namespace lumos
