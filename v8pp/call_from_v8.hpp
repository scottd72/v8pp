//
// Copyright (c) 2013-2016 Pavel Medvedev. All rights reserved.
//
// This file is part of v8pp (https://github.com/pmed/v8pp) project.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef V8PP_CALL_FROM_V8_HPP_INCLUDED
#define V8PP_CALL_FROM_V8_HPP_INCLUDED

#include <functional>
#include <sstream>

#include <v8.h>

#include "v8pp/convert.hpp"
#include "v8pp/utility.hpp"
#include "v8pp/call_from_v8_new.hpp"

namespace v8pp { namespace detail {

template<typename F, size_t Offset = 0>
struct call_from_v8_traits
{
	//static bool const is_mem_fun = std::is_member_function_pointer<F>::value;
	using arguments = typename function_traits<F>::arguments;

	//static size_t const arg_count =
	//std::tuple_size<arguments>::value - is_mem_fun - Offset;

	static size_t const arg_count =
		std::tuple_size<arguments>::value - Offset;

	static size_t const arg_count_using_this =
		arg_count - 1;
	
	template<size_t Index, bool>
	struct tuple_element
	{
		using type = typename std::tuple_element<Index, arguments>::type;
	};

	template<size_t Index>
	struct tuple_element<Index, false>
	{
		using type = void;
	};

	//template<size_t Index>
	//using arg_type = typename tuple_element<Index + is_mem_fun,
	//	Index < (arg_count + Offset)>::type;

	template<size_t Index>
	using arg_type = typename tuple_element<Index,
		Index < (arg_count + Offset)>::type;

	template <size_t Index>
	using arg_type_using_js_this = typename tuple_element<Index + 1,
	  Index < (arg_count + Offset)>::type;

	template<size_t Index>
	using convert_type = decltype(convert<arg_type<Index>>::from_v8(
	  std::declval<v8::Isolate*>(), std::declval<v8::Handle<v8::Value>>()));

	template<size_t Index>
	using convert_type_using_js_this =
		decltype(convert<arg_type_using_js_this<Index>>::from_v8(
		std::declval<v8::Isolate*>(), std::declval<v8::Handle<v8::Value>>()));
	
	template<size_t Index>
	static convert_type<Index>
	arg_from_v8(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		return convert<arg_type<Index>>::from_v8(args.GetIsolate(), args[Index - Offset]);
	}

	template <size_t Index>
	static convert_type_using_js_this<Index>
	arg_from_v8_using_js_this(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		return convert<arg_type_using_js_this<Index>>::
			from_v8(args.GetIsolate(), args[Index - Offset]);
	}
	
	static void check(v8::FunctionCallbackInfo<v8::Value> const& args,
										bool use_js_this)
	{
		size_t correct_arg_count = (use_js_this ? arg_count_using_this : arg_count);
		if (args.Length() != correct_arg_count)
		{
			throw std::runtime_error("argument count does not match function definition");
		}
	}
};

template<typename F>
using isolate_arg_call_traits = call_from_v8_traits<F, 1>;

template<typename F, size_t Offset = 0>
struct v8_args_call_traits : call_from_v8_traits<F, Offset>
{
	template<size_t Index>
	using arg_type = v8::FunctionCallbackInfo<v8::Value> const&;

	template<size_t Index>
	using arg_type_using_js_this = v8::FunctionCallbackInfo<v8::Value> const&;

	template<size_t Index>
	using convert_type = v8::FunctionCallbackInfo<v8::Value> const&;

	template<size_t Index>
	using convert_type_using_js_this = v8::FunctionCallbackInfo<v8::Value> const&;

	template<size_t Index>
	static v8::FunctionCallbackInfo<v8::Value> const&
	arg_from_v8(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		return args;
	}

	static void check(v8::FunctionCallbackInfo<v8::Value> const&,
										bool use_js_this)
	{
	}
};

template<typename F>
using isolate_v8_args_call_traits = v8_args_call_traits<F, 1>;

template<typename F, size_t Offset>
using is_direct_args = std::integral_constant<bool,
	call_from_v8_traits<F>::arg_count == (Offset + 1) &&
	std::is_same<typename call_from_v8_traits<F>::template arg_type<Offset>,
		v8::FunctionCallbackInfo<v8::Value> const&>::value>;

template<typename F>
using is_first_arg_isolate = std::integral_constant<bool,
	call_from_v8_traits<F>::arg_count != 0 &&
        convert_isolate<typename call_from_v8_traits<F>::
                        template arg_type<0>>::convertible::value>;                                                    
		/*
template<typename F>
using is_first_arg_isolate = std::integral_constant
	<bool, call_from_v8_traits<F>::arg_count != 0 &&
	 convert_isolate<typename std::remove_reference
									 <typename call_from_v8_traits<F>::
										template arg_type<0>>>::convertible::value>;                   
		*/
    
template<typename F>
using select_call_traits = typename std::conditional<is_first_arg_isolate<F>::value,
	typename std::conditional<is_direct_args<F, 1>::value,
		isolate_v8_args_call_traits<F>, isolate_arg_call_traits<F>>::type,
	typename std::conditional<is_direct_args<F, 0>::value,
		v8_args_call_traits<F>, call_from_v8_traits<F>>::type
>::type;

template<typename F, typename CallTraits, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	CallTraits, index_sequence<Indices...>)
{
	return func(CallTraits::template arg_from_v8<Indices>(args)...);
}

template<typename T, typename F, typename CallTraits, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	CallTraits, index_sequence<Indices...>)
{
	return (obj.*func)(CallTraits::template arg_from_v8_using_js_this
										 <Indices>(args)...);
}

// Added
template<typename T, typename F, typename CallTraits, size_t ...Indices>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this_impl
(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	CallTraits, index_sequence<Indices...>)
{
	return func(obj, CallTraits::template arg_from_v8_using_js_this
										 <Indices>(args)...);
}



template<typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_arg_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::template arg_type<0>>::
		from_isolate(args.GetIsolate());
	return func
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type<0>>::
		 arg_for_call_from_v8(converted_isolate), 
		 isolate_arg_call_traits<F>::template 
		 arg_from_v8<Indices + 1>(args)...);
}

template<typename T, typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_arg_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::template arg_type<0>>::
		from_isolate(args.GetIsolate());
	return (obj.*func)
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type<0>>::
		 arg_for_call_from_v8(converted_isolate),
		 isolate_arg_call_traits<F>::template arg_from_v8<Indices + 1>
		 (args)...);
}

// Added
template<typename T, typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this_impl
(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_arg_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::
										template arg_type_using_js_this<0>>::
		from_isolate(args.GetIsolate());
	return func
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type_using_js_this<0>>::
		 arg_for_call_from_v8(converted_isolate),
		 obj,
		 isolate_arg_call_traits<F>::template arg_from_v8_using_js_this<Indices + 1>
		 (args)...);
}



template<typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_v8_args_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::template arg_type<0>>::
		from_isolate(args.GetIsolate());
	return func
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type<0>>::
		 arg_for_call_from_v8(converted_isolate), args);
}

template<typename T, typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_from_v8_impl(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_v8_args_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::template arg_type<0>>::
		from_isolate(args.GetIsolate());
	return (obj.*func)
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type<0>>::
		 arg_for_call_from_v8(converted_isolate), args);
}

// Added
template<typename T, typename F, size_t ...Indices>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this_impl
(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	isolate_v8_args_call_traits<F>, index_sequence<Indices...>)
{
	auto converted_isolate = 
		convert_isolate<typename call_from_v8_traits<F>::template arg_type<0>>::
		from_isolate(args.GetIsolate());
	return func
		(convert_isolate
		 <typename call_from_v8_traits<F>::template arg_type<0>>::
		 arg_for_call_from_v8(converted_isolate), obj, args);
}


/*
template<typename F>
typename function_traits<F>::return_type
call_from_v8(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using call_traits = select_call_traits<F>;
	call_traits::check(args, false);
	return call_from_v8_impl(std::forward<F>(func), args,
		call_traits(), make_index_sequence<call_traits::arg_count>());
}
*/

template <typename F>
typename function_traits<F>::return_type
call_from_v8(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	return call_from_v8_new(std::forward<F>(func), args);
}

/*
template<typename T, typename F>
typename function_traits<F>::return_type
call_from_v8(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using call_traits = select_call_traits<F>;
	call_traits::check(args, true);
	return call_from_v8_impl(obj, std::forward<F>(func), args,
		call_traits(), make_index_sequence<call_traits::arg_count_using_this>());
}
*/

template <typename T, typename F>
typename function_traits<F>::return_type
call_from_v8(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	return call_from_v8_new(obj, std::forward<F>(func), args);
}

/*
// Added
template<typename T, typename F>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this
(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using call_traits = select_call_traits<F>;
	call_traits::check(args, true);
	return call_noncppmethod_from_v8_with_js_this_impl
		(obj, std::forward<F>(func), args,
		call_traits(), make_index_sequence<call_traits::arg_count_using_this>());
}
*/

template <typename T, typename F>
typename function_traits<F>::return_type
call_noncppmethod_from_v8_with_js_this
(T& obj, F&& func, v8::FunctionCallbackInfo<v8::Value> const& args)
{
	return call_noncppmethod_from_v8_with_js_this_new
		(obj, std::forward<F>(func), args);
}

/*
template <typename F>
typename function_traits<F>::return_type
call_cppmethod_from_v8_as_js_function
(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args) {
	return call_cppmethod_from_v8_as_js_function_new
		(std::forward<F>(func), args);
}
*/

}} // v8pp::detail

#endif // V8PP_CALL_FROM_V8_HPP_INCLUDED
