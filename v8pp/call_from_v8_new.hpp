// Copyright 2017 Scott Davies. All rights reserved.
// 
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

/*
	Complete rewrite of v8pp's version, for a few related reasons: 
  * Gets rid of all position dependence of arguments, other than 
    "first argument becomes C++ 'this' when calling C++ methods".
	* Allows users to bind C++ methods with parameters that don't correspond
    directly to JS arguments and/or require some setup before they're passed
    to C++ and/or require cleanup after the C++ method is complete. 
    (Example: bind C++ code that uses ErrorTrace* function parameters to 
    notify of errors instead of throwing C++ exceptions. An ErrorTrace can 
    be locally allocated, a pointer to it passed in to the wrapped method/
    function, and then when the call is complete the "cleanup" code can 
    check whether an error was recorded in the ErrorTrace and if so throw 
    an exception.) This can all be done by providing a specialization of
    call_from_v8_cpp_param_type_info below. (This is also now how 
    C++ arguments corresponding to the v8 Isolate are now handled, thus
    eliminating the clunky old position-dependent "call_traits" stuff.)
	* Allows C++ functions that aren't methods to be bound as JS methods.

 */

#ifndef V8PP_CALL_FROM_V8_NEW_HPP_INCLUDED
#define V8PP_CALL_FROM_V8_NEW_HPP_INCLUDED

#include <functional>
#include <sstream>
#include <v8.h>

#include "v8pp/convert.hpp"
#include "v8pp/utility.hpp"

namespace v8pp { 

/*
  Used for two mostly-unrelated things below:
  (1) Storing "nothing" when we don't need special handling for
      a particular argument type. (Probably actually requires one byte
      of dummy storage in most contexts, because C++ demands every element
      of a structure/array has a unique byte address, or something like that.)
  (2) Working around syntactic limitations on parameter unpacking
      via the use of initializer lists for a dummy constructor
*/
struct empty {
	template <typename... X>
	empty(X&&... x) {}
};

// Outside detail namespace because clients might want to specialize these
	
template <typename cpp_param_type_, typename enable = void>
struct call_from_v8_cpp_param_type_info {

	using cpp_param_type = cpp_param_type_;
	
	using preparation_type = empty;

	static const int v8_param_index_advance = 1;
	
	static preparation_type prepare
	(size_t /*v8_param_index*/,
	 v8::FunctionCallbackInfo<v8::Value> const& /*args*/) {
		return empty();
	}

	using get_param_type =
		decltype(convert<cpp_param_type>::from_v8
						 (std::declval<v8::Isolate*>(),
							std::declval<v8::Handle<v8::Value>>()));
	
  static //cpp_param_type
	get_param_type
	get_param(size_t v8_param_index,
						v8::FunctionCallbackInfo<v8::Value> const& args,
						preparation_type& /*prep*/) {
		return convert<cpp_param_type>::from_v8(args.GetIsolate(),
																						args[v8_param_index]);
	}

	static void cleanup_after_call(preparation_type& /* prep */) {}
};

template <typename T>
using is_direct_v8_args_type =
	std::is_same<T, v8::FunctionCallbackInfo<v8::Value> const&>;

template <typename T>
using is_isolate_type = std::integral_constant
	<bool, convert_isolate<T>::convertible::value>;

	
template <typename cpp_param_type_>
struct call_from_v8_cpp_param_type_info
<cpp_param_type_,
 typename std::enable_if
 <is_direct_v8_args_type<cpp_param_type_>::value>::type> {

	using cpp_param_type = cpp_param_type_;

	using preparation_type = empty;

	static const int v8_param_index_advance = 0;
	
	static preparation_type prepare(size_t /* v8_param_index */,
													 v8::FunctionCallbackInfo<v8::Value> const& args) {
		return empty();
	}

	static cpp_param_type get_param(size_t /* v8_param_index */,
													 v8::FunctionCallbackInfo<v8::Value> const& args,
													 preparation_type& /* prep */) {
		return args;
	}

	static void cleanup_after_call(preparation_type& /* prep */) {}	
};

template <typename cpp_param_type_>
struct call_from_v8_cpp_param_type_info
<cpp_param_type_,
 typename std::enable_if
 <is_isolate_type<cpp_param_type_>::value>::type> {

	using cpp_param_type = cpp_param_type_;

	using preparation_type = typename std::decay<typename
		detail::function_traits
		<decltype(convert_isolate<cpp_param_type>::from_isolate)>::
																			return_type>::type;

	static const int v8_param_index_advance = 0;

	static preparation_type prepare(size_t /* v8_param_index */,
													 v8::FunctionCallbackInfo<v8::Value> const& args) {
		return convert_isolate<cpp_param_type>::from_isolate(args.GetIsolate());
	}

	static cpp_param_type get_param(size_t /* v8_param_index */,
													 v8::FunctionCallbackInfo<v8::Value> const& args,
													 preparation_type& prep) {
		return convert_isolate<cpp_param_type>::arg_for_call_from_v8(prep);
	}

	static void cleanup_after_call(preparation_type& /* prep */) {}
	
};
	
}; // end namespace v8pp
	
namespace v8pp { namespace detail {

/* First, some additional tools for dealing with IntegerSequences
   (TODO: move these to utility.hpp or elsewhere ) */
template <size_t N, typename IntegerSequence>
struct get_integer_sequence_value;

template <typename T, T first, T... rest>
struct get_integer_sequence_value<0, integer_sequence<T, first, rest...>> {
	static const T value = first;
};
		
template <size_t N, typename T, T first, T... rest>
struct get_integer_sequence_value<N, integer_sequence<T, first, rest...>>:
		get_integer_sequence_value<N-1, integer_sequence<T, rest...>> {};

/*    
template <typename IntegerSequence>
struct integer_sequence_tail;

template <typename T, T Head, T... Tail>
struct integer_sequence_tail<integer_sequence<T, Head, Tail...>> {
	using type = integer_sequence<T, Tail...>;
};

template <typename IntegerSequence, typename IntegerSequence::type v>
struct integer_sequence_element_adder;

template <typename T, T to_add, T...values>
struct integer_sequence_element_adder<integer_sequence<T, values...>, to_add> {
	using type = integer_sequence<T, values+to_add ...>;
};
*/
    
/*
  v8_arg_index_computer: based on the argument types of a function,
  computes an index sequence where the nth element of the sequence tells
  you the index of the JS argument it corresponds to, if any. 

  This is not just the identity function because some arguments of the
  function may not correspond to any arguments incoming in the JS args.
  (This is what call_from_v8_cpp_param_type_info::v8_param_index_advance
  is used for.)
*/
		
template <size_t num_v8_args_so_far, typename v8_args_index_sequence_so_far,
          size_t next_real_v8_arg_index, typename remaining_fn_args_tuple,
					typename enable = void>
struct v8_arg_index_computer;

// Base case: remaining_fn_args_tuple is empty
template <size_t num_v8_args_so_far_,
					typename v8_args_sequence_so_far, size_t next_real_v8_arg_index>
struct v8_arg_index_computer<num_v8_args_so_far_, v8_args_sequence_so_far,
                             next_real_v8_arg_index, std::tuple<>>
{
  using v8_args_index_sequence = v8_args_sequence_so_far;
	static const size_t num_v8_args = num_v8_args_so_far_;
};

// Recursive case
template <size_t num_v8_args_so_far_, typename v8_args_sequence_so_far,
          size_t next_real_v8_arg_index, typename fn_next_arg_type,
          typename ...fn_remaining_arg_types>
struct v8_arg_index_computer<num_v8_args_so_far_,
														 v8_args_sequence_so_far, next_real_v8_arg_index,
                             std::tuple<fn_next_arg_type,
                                        fn_remaining_arg_types...>
                             > {
	using new_v8_args_sequence_so_far =
		typename v8_args_sequence_so_far::template append<next_real_v8_arg_index>;
	static const int next_next_real_v8_arg_index =
		next_real_v8_arg_index +
		call_from_v8_cpp_param_type_info<fn_next_arg_type>::v8_param_index_advance;
	static const size_t old_num_v8_args_so_far = num_v8_args_so_far_;
	static const size_t num_v8_args_so_far = old_num_v8_args_so_far +
		call_from_v8_cpp_param_type_info<fn_next_arg_type>::v8_param_index_advance;

	using recursive_type = v8_arg_index_computer
		<num_v8_args_so_far,
		 new_v8_args_sequence_so_far,
		 next_next_real_v8_arg_index,
		 std::tuple<fn_remaining_arg_types...>>;
	
	using v8_args_index_sequence =
		 typename recursive_type::v8_args_index_sequence;

	static const size_t num_v8_args = recursive_type::num_v8_args;
	
};

/* Note: an index in the sequence may actually be unused when arguments
   for a function/method are actually extracted. Typically this will be the
   case when the *next* index in the sequence has the same value. */
template <typename arg_tuple>
struct v8_arg_indices {
	using index_computer = v8_arg_index_computer<0, integer_sequence<int>, 0,
																							 arg_tuple>;
	static const size_t num_v8_args =
		index_computer::num_v8_args;
	using function_sequence = typename index_computer::v8_args_index_sequence;

};

// A hack to get around C++'s crappy handling of "void" types when
// metaprogramming on functions/methods whose return values might or might
// not be void.
//
// "Borrowed" from Corey Kosak with permission and a couple of bug fixes.

// result_holder for Lambdas which do not return void
template<typename Lambda, typename Result>
class result_holder {
public:
  result_holder(Lambda lambda) : result_(lambda()) {}

  Result &&result() { return std::forward<Result>(result_); }

private:
  Result result_;
};

// Specialization: result_holder for Lambdas which return void
template<typename Lambda>
class result_holder<Lambda, void> {
public:
  result_holder(Lambda lambda) { lambda(); }
  void result() { }
};

// Usage: auto res = result_saver([...]{ return f(...); })
//        ...   		
//        return res.result();
template<typename Lambda>
auto result_saver(Lambda lambda) -> result_holder<Lambda, decltype(lambda())> {
  return result_holder<Lambda, decltype(lambda())>(std::move(lambda));
}

		
template <typename result_type_, typename param_tuple_type_,
  				typename ParamIndexSequence>
struct call_from_v8_helper;

template <typename result_type_, typename param_tuple_type_,
					int... ParamIndices>
struct call_from_v8_helper<result_type_, param_tuple_type_,
													 index_sequence<ParamIndices...>> {

	using result_type = result_type_;
	using param_tuple_type = param_tuple_type_;
	using v8_function_arg_index_sequence =
		typename v8_arg_indices<param_tuple_type>::function_sequence;

	static const size_t num_v8_args =
		v8_arg_indices<param_tuple_type>::num_v8_args;
	
	using prepare_tuple_type = std::tuple
		<typename call_from_v8_cpp_param_type_info
		 <typename std::tuple_element<ParamIndices,
																	param_tuple_type>::type>::
		 preparation_type ...>;

  static void cleanup(prepare_tuple_type& pt) {
		empty
			{(
				call_from_v8_cpp_param_type_info
				<typename std::tuple_element<ParamIndices,
				param_tuple_type>::type>::
				cleanup_after_call(std::get<ParamIndices>(pt)),
				false)...
					};
	}

	static void check_arg_count(size_t correct_arg_count,
											 v8::FunctionCallbackInfo<v8::Value> const& args) {
		if (correct_arg_count != args.Length()) {
			std::ostringstream oss;
			oss << "Count of provided arguments (" << args.Length() <<
				") does not match count of required arguments (" <<
				correct_arg_count << ")";
			throw std::runtime_error(oss.str());
		}
	}

	/*
   In this case, the mapping from ParamIndices to v8 arg indices 
   is the identity except when there are special argument types,
   i.e. the v8 arg indices will start with 0.
	 */
	template <typename FF>
	static result_type call_function
	(FF&& func, v8::FunctionCallbackInfo<v8::Value> const& args) {

		check_arg_count(num_v8_args, args);
		
		prepare_tuple_type prepare_tuple
			(call_from_v8_cpp_param_type_info
			 <typename std::tuple_element<ParamIndices,
			                              param_tuple_type>::type>::
			 prepare(get_integer_sequence_value
							 <ParamIndices, v8_function_arg_index_sequence>::value, args)...);

		auto res = result_saver
			([&func,&prepare_tuple,&args]() -> result_type
			 { return func
				 (call_from_v8_cpp_param_type_info
					<typename std::tuple_element<ParamIndices,
					param_tuple_type>::type>::
					get_param
					(get_integer_sequence_value
					 <ParamIndices, v8_function_arg_index_sequence>::value, args,
					 std::get<ParamIndices>(prepare_tuple)) ...); });

		cleanup(prepare_tuple);

		return res.result();
	}

	/* 
		 In this case, obj has already been extracted from args.This().
     We have also already removed the head containing the type of obj 
		 from the param_tuple_type template parameter. This means
     the first parameter to the method call (not including obj)
     should generally use the v8 arg with index 0.
	 */
	template <typename T, typename FF>
	static result_type call_method
	(T& obj, FF&& method, v8::FunctionCallbackInfo<v8::Value> const& args) {

		check_arg_count(num_v8_args, args);
		
		prepare_tuple_type prepare_tuple
			(call_from_v8_cpp_param_type_info
			 <typename std::tuple_element<ParamIndices,
			 param_tuple_type>::type>::
			 prepare(get_integer_sequence_value
							 <ParamIndices, v8_function_arg_index_sequence>::value, args)...);

		auto res = result_saver
			([&obj,&method,&prepare_tuple,&args]() -> result_type
			 { return (obj.*method)
				 (call_from_v8_cpp_param_type_info
					<typename std::tuple_element<ParamIndices, param_tuple_type>::type>::
					get_param
					(get_integer_sequence_value
					 <ParamIndices, v8_function_arg_index_sequence>::value, args,
					 std::get<ParamIndices>(prepare_tuple)) ...);
			 });

		cleanup(prepare_tuple);

		return res.result();
		
	}

	/*
		Same as call_method, except rather than (obj.*method)(...)
    we call func(obj, ...). 
	 */
	template <typename T, typename F>
	static result_type call_noncppmethod_with_js_this
	(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args) {

		check_arg_count(num_v8_args, args);
		
		prepare_tuple_type prepare_tuple
			(call_from_v8_cpp_param_type_info
			 <typename std::tuple_element<ParamIndices,
			 param_tuple_type>::type>::
			 prepare(get_integer_sequence_value
							 <ParamIndices, v8_function_arg_index_sequence>::value, args)...);

		auto res = result_saver
			([&obj,&func,&prepare_tuple,&args]() ->result_type
			 { return func
				 (obj, 
					call_from_v8_cpp_param_type_info
					<typename std::tuple_element<ParamIndices, param_tuple_type>::type>::
					get_param
					(get_integer_sequence_value
					 <ParamIndices, v8_function_arg_index_sequence>::value, args,
					 std::get<ParamIndices>(prepare_tuple)) ...);
			 });

		cleanup(prepare_tuple);

		return res.result();
		
	}

	template <typename T, typename F>
	static result_type call_noncppmethod_with_js_this
	(std::shared_ptr<T> obj, F&& func,
	 v8::FunctionCallbackInfo<v8::Value> const& args) {

		check_arg_count(num_v8_args, args);
		
		prepare_tuple_type prepare_tuple
			(call_from_v8_cpp_param_type_info
			 <typename std::tuple_element<ParamIndices,
			 param_tuple_type>::type>::
			 prepare(get_integer_sequence_value
							 <ParamIndices, v8_function_arg_index_sequence>::value, args)...);

		auto res = result_saver
			([&obj,&func,&prepare_tuple,&args]() ->result_type
			 { return func
				 (obj, 
					call_from_v8_cpp_param_type_info
					<typename std::tuple_element<ParamIndices, param_tuple_type>::type>::
					get_param
					(get_integer_sequence_value
					 <ParamIndices, v8_function_arg_index_sequence>::value, args,
					 std::get<ParamIndices>(prepare_tuple)) ...);
			 });

		cleanup(prepare_tuple);

		return res.result();
		
	}
	
};

template <typename F>
typename function_traits<F>::return_type
call_from_v8_new(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	constexpr size_t num_args =
		std::tuple_size<typename detail::function_traits<F>::arguments>::value;
	return call_from_v8_helper
		<typename function_traits<F>::return_type,
		 typename function_traits<F>::arguments,
		 make_index_sequence<num_args>>::call_function(std::forward<F>(func), args);
}

template <typename T, typename F>
typename function_traits<F>::return_type
call_from_v8_new(T&& obj, F&& func,
								 v8::FunctionCallbackInfo<v8::Value> const& args)
{
	constexpr size_t num_args =
		std::tuple_size<typename detail::function_traits<F>::arguments>::value-1;
	return call_from_v8_helper
		<typename function_traits<F>::return_type,
		 typename tuple_tail<typename function_traits<F>::arguments>::type,
		 make_index_sequence<num_args>>::call_method
		(std::forward<T>(obj), std::forward<F>(func), args);
}

template <typename T, typename F>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this_new
(T&& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	constexpr size_t num_args =
		std::tuple_size<typename detail::function_traits<F>::arguments>::value-1;
	return call_from_v8_helper
		<typename function_traits<F>::return_type,
		 typename tuple_tail<typename function_traits<F>::arguments>::type,
		 make_index_sequence<num_args>>::call_noncppmethod_with_js_this
		(std::forward<T>(obj), std::forward<F>(func), args);
}


/* for debugging only */		
template <typename integer_sequence>
class integer_sequence_to_string;

template <typename integer_type>
class integer_sequence_to_string<integer_sequence<integer_type>> {
public:

	static std::string get_string() {
		std::ostringstream oss;
		oss << "[ ";
		add(oss);
		oss << "]";
		return oss.str();
	}
	
  static void add(std::ostream& os) { };
};

template <typename integer_type, integer_type i0,
  				integer_type... IR>
class integer_sequence_to_string<integer_sequence<integer_type, i0, IR...>> {
public:

	static std::string get_string() {
		std::ostringstream oss;
		oss << "[ ";
		add(oss);
		oss << "]";
		return oss.str();
	}
	
	static void add(std::ostream& os) {
		os << i0;
		os << " ";
		integer_sequence_to_string<v8pp::detail::
															 integer_sequence<integer_type, IR...>>::add(os);
	}
};

/* end stuff for debugging */
		
}} // v8pp::detail

#endif
