//
// Copyright (c) 2013-2016 Pavel Medvedev. All rights reserved.
//
// This file is part of v8pp (https://github.com/pmed/v8pp) project.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef V8PP_FACTORY_HPP_INCLUDED
#define V8PP_FACTORY_HPP_INCLUDED

#include <memory>
#include <utility>

#include <v8.h>

namespace v8pp {

// Factory that creates new C++ objects of type T
template<typename T>
struct factory
{
	static size_t const object_size = sizeof(T);

	template<typename ...Args>
	static T* create(v8::Isolate* isolate, Args... args)
	{
		T* object = new T(std::forward<Args>(args)...);
		isolate->AdjustAmountOfExternalAllocatedMemory(
			static_cast<int64_t>(object_size));
		return object;
	}

	static void destroy(v8::Isolate* isolate, T* object)
	{
		delete object;
		isolate->AdjustAmountOfExternalAllocatedMemory(
			-static_cast<int64_t>(object_size));
	}
};

/*
Objects embedded with shared_ptrs are handled differently because:

1) They may stick around longer than any isolate in which they're embedded,
in which case any reference/pointer to that isolate at object destruction 
time is going to be garbage

(2) Their memory probably shouldn't be counted against the amount of 
externally allocated memory "owned" by the isolate anyways.
*/

template <typename T>
struct shared_object_factory
{
	template <typename ...Args>
	static std::shared_ptr<T> create(Args... args)
	{
		return std::shared_ptr<T>(new T(std::forward<Args>(args)...));
	}
};

} //namespace v8pp

#endif // V8PP_FACTORY_HPP_INCLUDED
