//
// Copyright (c) 2013-2016 Pavel Medvedev. All rights reserved.
//
// This file is part of v8pp (https://github.com/pmed/v8pp) project.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Some modifications / additions by Scott Davies.

#ifndef V8PP_CONVERT_HPP_INCLUDED
#define V8PP_CONVERT_HPP_INCLUDED

#include <v8.h>

#include <climits>
#include <string>
#include <array>
#include <vector>
#include <map>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>

namespace v8pp {

template<typename T>
class class_;

// Generic convertor
template<typename T, typename Enable = void>
struct convert;
/*
{
	using from_type = T;
	using to_type = v8::Handle<v8::Value>;

	static bool is_valid(v8::Isolate* isolate, v8::Handle<v8::Value> value);

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value);
	static to_type to_v8(v8::Isolate* isolate, T const& value);
};
*/

// converter specializations for string types
template<typename Char, typename Traits, typename Alloc>
struct convert<std::basic_string<Char, Traits, Alloc>>
{
	static_assert(sizeof(Char) <= sizeof(uint16_t),
		"only UTF-8 and UTF-16 strings are supported");

	using from_type = std::basic_string<Char, Traits, Alloc>;
	using to_type = v8::Handle<v8::String>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsString();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected String");
		}

		if (sizeof(Char) == 1)
		{
			v8::String::Utf8Value const str(isolate, value);
			return from_type(reinterpret_cast<Char const*>(*str), str.length());
		}
		else
		{
			v8::String::Value const str(isolate, value);
			return from_type(reinterpret_cast<Char const*>(*str), str.length());
		}
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		if (sizeof(Char) == 1)
		{
			return v8::String::NewFromUtf8(isolate, reinterpret_cast<char const*>(value.data()),
				v8::String::kNormalString, static_cast<int>(value.length()));
		}
		else
		{
			return v8::String::NewFromTwoByte(isolate, reinterpret_cast<uint16_t const*>(value.data()),
				v8::String::kNormalString, static_cast<int>(value.length()));
		}
	}
};

template <typename Char>
struct array_usable_as_string
{
	static const bool value = false;
};

template <>
struct array_usable_as_string<char>
{
	static const bool value = true;
};

template <>
struct array_usable_as_string<unsigned char>
{
	static const bool value = true;
};

template <>
struct array_usable_as_string<uint16_t>
{
	static const bool value = true;
};

	
template<typename Char>
struct convert<Char const*,
							 typename std::enable_if
							 <array_usable_as_string<Char>::value>::type>
{
	static_assert(sizeof(Char) <= sizeof(uint16_t),
		"only UTF-8 and UTF-16 strings are supported");


	// A string that converts to Char const *
	struct convertible_string : std::basic_string<Char>
	{
		convertible_string(Char const* str, size_t len) : std::basic_string<Char>(str, len) {}
		operator Char const*() const { return this->c_str(); }
	};

	using from_type = convertible_string;
	using to_type = v8::Handle<v8::String>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsString();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected String");
		}

		if (sizeof(Char) == 1)
		{
			v8::String::Utf8Value const str(isolate, value);
			return from_type(reinterpret_cast<Char const*>(*str), str.length());
		}
		else
		{
			v8::String::Value const str(isolate, value);
			return from_type(reinterpret_cast<Char const*>(*str), str.length());
		}
	}

	static to_type to_v8(v8::Isolate* isolate, Char const* value, size_t len = ~0)
	{
		if (sizeof(Char) == 1)
		{
			return v8::String::NewFromUtf8(isolate, reinterpret_cast<char const*>(value),
				v8::String::kNormalString, static_cast<int>(len));
		}
		else
		{
			return v8::String::NewFromTwoByte(isolate, reinterpret_cast<uint16_t const*>(value),
				v8::String::kNormalString, static_cast<int>(len));
		}
	}
};

// converter specializations for primitive types
template<>
struct convert<bool>
{
	using from_type = bool;
	using to_type = v8::Handle<v8::Boolean>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsBoolean();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Boolean");
		}
		return value->ToBoolean(isolate)->Value();
	}

	static to_type to_v8(v8::Isolate* isolate, bool value)
	{
		return v8::Boolean::New(isolate, value);
	}
};

template<typename T>
struct convert<T, typename std::enable_if<std::is_integral<T>::value>::type>
{
	using from_type = T;
	using to_type = v8::Handle<v8::Number>;

	enum { bits = sizeof(T) * CHAR_BIT, is_signed = std::is_signed<T>::value };

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Number");
		}

		if (bits <= 32)
		{
			if (is_signed)
			{
				return static_cast<T>(value->Int32Value(isolate->GetCurrentContext()).
															FromJust());
			}
			else
			{
				return static_cast<T>(value->Uint32Value(isolate->GetCurrentContext()).
															FromJust());
			}
		}
		else
		{
			return static_cast<T>(value->IntegerValue(isolate->GetCurrentContext()).
														FromJust());
		}
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		if (bits <= 32)
		{
			if (is_signed)
			{
				return v8::Integer::New(isolate, static_cast<int32_t>(value));
			}
			else
			{
				return v8::Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(value));
			}
		}
		else
		{
			//TODO: check value < (1<<57) to fit in double?
			return v8::Number::New(isolate, static_cast<double>(value));
		}
	}
};

template<typename T>
struct convert<T, typename std::enable_if<std::is_enum<T>::value>::type>
{
	using underlying_type = typename std::underlying_type<T>::type;

	using from_type = T;
	using to_type = typename convert<underlying_type>::to_type;

	static bool is_valid(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return convert<underlying_type>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return static_cast<T>(convert<underlying_type>::from_v8(isolate, value));
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		return convert<underlying_type>::to_v8(isolate, static_cast<underlying_type>(value));
	}
};

template<typename T>
struct convert<T, typename std::enable_if<std::is_floating_point<T>::value>::type>
{
	using from_type = T;
	using to_type = v8::Handle<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Number");
		}

		return static_cast<T>(value->NumberValue(isolate->GetCurrentContext()).
													FromJust());
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		return v8::Number::New(isolate, value);
	}
};

// convert Array <-> std::array
template<typename T, size_t N>
struct convert<std::array<T, N>>
{
	using from_type = std::array<T, N>;
	using to_type = v8::Handle<v8::Array>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Array");
		}

		v8::HandleScope scope(isolate);
		v8::Local<v8::Array> array = value.As<v8::Array>();

		if (array->Length() != N)
		{
			throw std::runtime_error("Invalid array length: expected " + std::to_string(N)
				+ " actual " + std::to_string(array->Length()));
		}

		from_type result;
		for (uint32_t i = 0; i < N; ++i)
		{
			result[i] = convert<T>::from_v8(isolate, array->Get(i));
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);

		v8::Local<v8::Array> result = v8::Array::New(isolate, N);
		for (uint32_t i = 0; i < N; ++i)
		{
			result->Set(i, convert<T>::to_v8(isolate, value[i]));
		}
		return scope.Escape(result);
	}
};

// convert Array <-> std::vector
template<typename T, typename Alloc>
struct convert<std::vector<T, Alloc>>
{
	using from_type = std::vector<T, Alloc>;
	using to_type = v8::Handle<v8::Array>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Array");
		}

		v8::HandleScope scope(isolate);
		v8::Local<v8::Array> array = value.As<v8::Array>();

		from_type result;
		result.reserve(array->Length());
		for (uint32_t i = 0, count = array->Length(); i < count; ++i)
		{
			result.emplace_back(convert<T>::from_v8(isolate, array->Get(i)));
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);

		uint32_t const size = static_cast<uint32_t>(value.size());
		v8::Local<v8::Array> result = v8::Array::New(isolate, size);
		for (uint32_t i = 0; i < size; ++i)
		{
			result->Set(i, convert<T>::to_v8(isolate, value[i]));
		}
		return scope.Escape(result);
	}
};

// convert Object <-> std::map
template<typename Key, typename Value, typename Less, typename Alloc>
struct convert<std::map<Key, Value, Less, Alloc>>
{
	using from_type = std::map<Key, Value, Less, Alloc>;
	using to_type = v8::Handle<v8::Object>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Object");
		}

		v8::HandleScope scope(isolate);
		v8::Local<v8::Object> object = value.As<v8::Object>();
		v8::Local<v8::Array> prop_names = object->GetPropertyNames();

		from_type result;
		for (uint32_t i = 0, count = prop_names->Length(); i < count; ++i)
		{
			v8::Local<v8::Value> key = prop_names->Get(i);
			v8::Local<v8::Value> val = object->Get(key);
			result.emplace(convert<Key>::from_v8(isolate, key), convert<Value>::from_v8(isolate, val));
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);

		v8::Local<v8::Object> result = v8::Object::New(isolate);
		for (auto const& item: value)
		{
			result->Set(convert<Key>::to_v8(isolate, item.first), convert<Value>::to_v8(isolate, item.second));
		}
		return scope.Escape(result);
	}
};

// converter specializations for V8 Handles
template<typename T>
struct convert<v8::Handle<T>, typename std::enable_if<
	!std::is_same<v8::Handle<T>, v8::Local<T>>::value>::type>
{
	using from_type = v8::Handle<T>;
	using to_type = v8::Handle<T>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.As<T>().IsEmpty();
	}

	static v8::Handle<T> from_v8(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return value.As<T>();
	}

	static v8::Handle<T> to_v8(v8::Isolate*, v8::Handle<T> value)
	{
		return value;
	}
};

template<typename T>
struct convert<v8::Local<T>>
{
	using from_type = v8::Handle<T>;
	using to_type = v8::Handle<T>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.As<T>().IsEmpty();
	}

	static v8::Handle<T> from_v8(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return value.As<T>();
	}

	static v8::Handle<T> to_v8(v8::Isolate*, v8::Local<T> value)
	{
		return value;
	}
};

template<typename T>
struct convert<v8::Persistent<T>>
{
	using from_type = v8::Handle<T>;
	using to_type = v8::Handle<T>;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.As<T>().IsEmpty();
	}

	static v8::Handle<T> from_v8(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return value.As<T>();
	}

	static v8::Handle<T> to_v8(v8::Isolate*, v8::Persistent<T> value)
	{
		return value;
	}
};

template<typename T, typename Enable = void>
struct is_wrapped_class;

// convert specialization for wrapped user classes
template<typename T>
struct is_wrapped_class<T> : std::is_class<T> {};

template <typename T>
struct is_wrapped_class<std::shared_ptr<T>>: std::false_type{};

template <typename T>
struct is_wrapped_class<T*>: std::false_type{};

template <typename T>
struct is_wrapped_class<T const*>: std::false_type{};

template <typename T>
struct is_wrapped_class<T&>: std::false_type{};

template <typename T>
struct is_wrapped_class<T const&>: std::false_type{};

template<typename T>
struct is_wrapped_class<v8::Handle<T>, typename std::enable_if<
	!std::is_same<v8::Handle<T>, v8::Local<T>>::value>::type> : std::false_type{};

template<typename T>
struct is_wrapped_class<v8::Local<T>> : std::false_type {};

template<typename T>
struct is_wrapped_class<v8::Persistent<T>> : std::false_type {};

template<typename Char, typename Traits, typename Alloc>
struct is_wrapped_class<std::basic_string<Char, Traits, Alloc>> : std::false_type {};

template<typename T, size_t N>
struct is_wrapped_class<std::array<T, N>> : std::false_type{};

template<typename T, typename Alloc>
struct is_wrapped_class<std::vector<T, Alloc>> : std::false_type {};

template<typename Key, typename Value, typename Less, typename Alloc>
struct is_wrapped_class<std::map<Key, Value, Less, Alloc>> : std::false_type {};

template<typename T>
struct convert<T*,
							 typename std::enable_if<is_wrapped_class<T>::value &&
																			 !std::is_const<T>::value>::type>
{
	using from_type = T*;
	using to_type = v8::Handle<v8::Object>;
	using class_type = typename std::remove_cv<T>::type;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			return nullptr;
		}
		return class_<class_type>::unwrap_object(isolate, value);
	}

	static to_type to_v8(v8::Isolate* isolate, T const* value)
	{
		return class_<class_type>::find_object(isolate, value);
	}
};

template<typename T>
struct convert<T const*,
							 typename std::enable_if<is_wrapped_class<T>::value>::type>
{
	using from_type = T const*;
	using to_type = v8::Handle<v8::Object>;
	using class_type = typename std::remove_cv<T>::type;

	static bool is_valid(v8::Isolate*, v8::Handle<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			return nullptr;
		}
		return class_<class_type>::unwrap_const_object(isolate, value);
	}

	static to_type to_v8(v8::Isolate* isolate, T const* value)
	{
		return class_<class_type>::find_object(isolate, value);
	}
};


template<typename T>
struct convert<T, typename std::enable_if<is_wrapped_class<T>::value>::type>
{
	using from_type = T&;
	using to_type = v8::Handle<v8::Object>;

	static bool is_valid(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return convert<T*>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Object");
		}
		if (T* object = convert<T*>::from_v8(isolate, value))
		{
			return *object;
		}
		throw std::runtime_error("expected C++ wrapped object");
	}

	static to_type to_v8(v8::Isolate* isolate, T const& value)
	{
		return convert<T*>::to_v8(isolate, &value);
	}
};

/*
template<typename T>
struct convert<const T,
							 typename std::enable_if<is_wrapped_class<T>::value>::type> :
	public convert<T> {};
*/
/*
template<typename T>
struct convert<const T,
							 typename std::enable_if<is_wrapped_class<T>::value>::type> {
};
*/

template <typename T>
struct convert<std::shared_ptr<T>, typename std::enable_if<is_wrapped_class<T>::value && !std::is_const<T>::value>::type>
{
	using from_type = std::shared_ptr<T>;
	using to_type = v8::Handle<v8::Object>;
	using class_type = T;

	static bool is_valid(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return convert<class_type*>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Object");
		}
		auto result = class_<class_type>::unwrap_shared_object(isolate, value);
		if (result.get() == nullptr)
		{
			throw std::runtime_error("expected C++ wrapped object with shared_ptr");
		}
		return result;
	}
	static to_type to_v8(v8::Isolate* isolate, from_type value)
	{
		return class_<class_type>::find_shared_object(isolate, value);
	}
};

template <typename T>
struct convert<std::shared_ptr<T const>, typename std::enable_if<is_wrapped_class<T>::value>::type>
{
	using from_type = std::shared_ptr<T const>;
	using to_type = v8::Handle<v8::Object>;
	using class_type = T;

	static bool is_valid(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return convert<class_type*>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::invalid_argument("expected Object");
		}
		auto result = class_<class_type>::unwrap_const_shared_object
			(isolate, value);
		if (result.get() == nullptr)
		{
			throw std::runtime_error("expected C++ wrapped object with shared_ptr");
		}
		return result;
	}
	static to_type to_v8(v8::Isolate* isolate, from_type value)
	{
		return class_<class_type>::find_const_shared_object(isolate, value);
	}
};

/*
template<typename T>
struct convert<T const*, typename std::enable_if<is_wrapped_class<T>::value>::type> {
};
*/	

template<typename T>
struct convert<T&> : convert<T> {};

template<typename T>
struct convert<T const&> : convert<T> {};


template <typename T, typename Enable = void>
struct convert_result_to_v8 {
	using from_type = typename convert<T>::from_type;
	using to_type = typename convert<T>::to_type;
	static to_type result_to_v8(v8::Isolate* isolate, from_type const& value) {
		return convert<T>::to_v8(isolate, value);
	}
};

template <typename T>
struct convert_result_to_v8
<T, typename std::enable_if<is_wrapped_class<T>::value
														&& !std::is_const<T>::value>::type> {
	using from_type = T;
	using class_type = typename std::remove_cv<T>::type;
	using to_type = typename convert<T>::to_type;
	static to_type result_to_v8(v8::Isolate* isolate, from_type& value) {
		class_type* mycopy(new class_type(std::move(value)));
		return class_<class_type>::import_external(isolate, mycopy);
	}
};

template <typename T>
struct convert_result_to_v8
<T const, typename std::enable_if<is_wrapped_class<T>::value>::type> {
	using from_type = T const;
	using class_type = typename std::remove_cv<T>::type;
	using to_type = typename convert<T>::to_type;
	static to_type result_to_v8(v8::Isolate* isolate, from_type& value) {
		class_type* mycopy(new class_type(std::move(value)));
		return class_<class_type>::import_const_external(isolate, mycopy);
	}
};

template<typename T>
auto from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	-> decltype(convert<T>::from_v8(isolate, value))
{
	return convert<T>::from_v8(isolate, value);
}

template<typename T, typename U>
auto from_v8(v8::Isolate* isolate, v8::Handle<v8::Value> value, U const& default_value)
	-> decltype(convert<T>::from_v8(isolate, value))
{
	return convert<T>::is_valid(isolate, value)? convert<T>::from_v8(isolate, value) : default_value;
}

inline v8::Handle<v8::String> to_v8(v8::Isolate* isolate, char const* str, size_t len)
{
	return convert<char const*>::to_v8(isolate, str, len);
}

template<size_t N>
v8::Handle<v8::String> to_v8(v8::Isolate* isolate, char const (&str)[N], size_t len = N - 1)
{
	return convert<char const*>::to_v8(isolate, str, len);
}

#ifdef WIN32
inline v8::Handle<v8::String> to_v8(v8::Isolate* isolate, wchar_t const* str, size_t len)
{
	return convert<wchar_t const*>::to_v8(isolate, str, len);
}

template<size_t N>
v8::Handle<v8::String> to_v8(v8::Isolate* isolate, wchar_t const (&str)[N], size_t len = N - 1)
{
	return convert<wchar_t const*>::to_v8(isolate, str, len);
}
#endif

template<typename T>
auto to_v8(v8::Isolate* isolate, T const& value)
	-> decltype(convert<T>::to_v8(isolate, value))
{
	return convert<T>::to_v8(isolate, value);
}

template<typename Iterator>
v8::Handle<v8::Array> to_v8(v8::Isolate* isolate, Iterator begin, Iterator end)
{
	v8::EscapableHandleScope scope(isolate);

	v8::Local<v8::Array> result = v8::Array::New(isolate);
	for (uint32_t idx = 0; begin != end; ++begin, ++idx)
	{
		result->Set(idx, to_v8(isolate, *begin));
	}
	return scope.Escape(result);
}

template<typename T>
v8::Handle<v8::Array> to_v8(v8::Isolate* isolate, std::initializer_list<T> const& init)
{
	return to_v8(isolate, init.begin(), init.end());
}

template<typename T>
v8::Local<T> to_local(v8::Isolate* isolate, v8::PersistentBase<T> const& handle)
{
	if (handle.IsWeak())
	{
		return v8::Local<T>::New(isolate, handle);
	}
	else
	{
		return *reinterpret_cast<v8::Local<T>*>(const_cast<v8::PersistentBase<T>*>(&handle));
	}
}

template <typename T>
auto result_to_v8(v8::Isolate* isolate, T&& value)
	-> decltype(convert_result_to_v8<T>::result_to_v8(isolate, value))
{
	return convert_result_to_v8<T>::result_to_v8(isolate, value);
}


template <typename T, typename Enable = void> struct convert_isolate
{
	using convertible = std::integral_constant<bool, false>;

	//   stored for later call to arg_for_call_from_v8 ... this is so
	//   from_isolate can return an object if the function parameter is a
	//   pointer. Returning a pointer that then got passed straight to the
	//   function would result in awkward object lifetime issues.
	// static T2 from_isolate(v8::Isolate* isolate); 

	// static T arg_for_call_from_v8(T2& t); // (or maybe just T2 t)
};

template <> struct convert_isolate<v8::Isolate*>
{
	using convertible = std::integral_constant<bool, true>;
	static v8::Isolate* from_isolate(v8::Isolate* isolate) { return isolate; }
  static v8::Isolate* arg_for_call_from_v8(v8::Isolate* i) { return i; }
};

template <class T> struct isolate_convertible
{
	static constexpr bool value = convert_isolate<T>::convertible::value;
};


} // namespace v8pp

#endif // V8PP_CONVERT_HPP_INCLUDED
