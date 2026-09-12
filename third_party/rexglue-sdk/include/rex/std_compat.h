#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef __cpp_lib_move_only_function
namespace std {

template <typename>
class move_only_function;

template <typename R, typename... Args>
class move_only_function<R(Args...)> {
 public:
  move_only_function() = default;
  move_only_function(nullptr_t) {}

  move_only_function(const move_only_function&) = delete;
  move_only_function& operator=(const move_only_function&) = delete;
  move_only_function(move_only_function&&) noexcept = default;
  move_only_function& operator=(move_only_function&&) noexcept = default;

  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, move_only_function> &&
             !std::is_same_v<std::remove_cvref_t<F>, nullptr_t> &&
             std::is_invocable_r_v<R, F&, Args...>)
  move_only_function(F&& f)
      : callable_(std::make_unique<Callable<std::remove_cvref_t<F>>>(std::forward<F>(f))) {}

  move_only_function& operator=(nullptr_t) {
    callable_.reset();
    return *this;
  }

  explicit operator bool() const noexcept { return callable_ != nullptr; }

  friend bool operator==(const move_only_function& f, nullptr_t) noexcept {
    return !static_cast<bool>(f);
  }
  friend bool operator==(nullptr_t, const move_only_function& f) noexcept {
    return !static_cast<bool>(f);
  }
  friend bool operator!=(const move_only_function& f, nullptr_t) noexcept {
    return static_cast<bool>(f);
  }
  friend bool operator!=(nullptr_t, const move_only_function& f) noexcept {
    return static_cast<bool>(f);
  }

  R operator()(Args... args) {
    return callable_->Call(std::forward<Args>(args)...);
  }

 private:
  struct CallableBase {
    virtual ~CallableBase() = default;
    virtual R Call(Args&&... args) = 0;
  };

  template <typename F>
  struct Callable final : CallableBase {
    explicit Callable(F&& f) : f_(std::move(f)) {}
    explicit Callable(const F& f) : f_(f) {}

    R Call(Args&&... args) override { return std::invoke(f_, std::forward<Args>(args)...); }

    F f_;
  };

  std::unique_ptr<CallableBase> callable_;
};

}  // namespace std
#endif
