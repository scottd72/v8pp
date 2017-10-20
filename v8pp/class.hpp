//
// Copyright (c) 2013-2016 Pavel Medvedev. All rights reserved.
//
// This file is part of v8pp (https://github.com/pmed/v8pp) project.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Extensive modifications and additions written by Scott Davies.
#ifndef V8PP_CLASS_HPP_INCLUDED
#define V8PP_CLASS_HPP_INCLUDED

#include <cstdio>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "v8pp/config.hpp"
//#include "v8pp/factory.hpp"
#include "v8pp/function.hpp"
#include "v8pp/persistent.hpp"
#include "v8pp/property.hpp"

namespace v8pp {

template<typename T>
class class_;

namespace detail {

template <typename Class, typename... Args>
struct function_for_constructor_helper {
  static Class* construct(Args... args) {
    return new Class(std::forward<Args>(args)...);
  }
  static std::shared_ptr<Class> construct_shared_ptr(Args...args) {
    return std::make_shared<Class>(std::forward<Args>(args)...);
  }
};

template <typename Class, typename... Args>
std::function<Class*(Args...)> get_function_for_constructor() {
  using ctor_type = Class* (*)(Args...);
  return std::function<Class*(Args...)>
    (static_cast<ctor_type>
     (&function_for_constructor_helper<Class, Args...>::construct));
}

template <typename Class, typename... Args>
std::function<std::shared_ptr<Class>(Args...)>
get_function_for_shared_ptr_from_constructor() {
  using fn_type = std::shared_ptr<Class>(*)(Args...);
  return std::function<std::shared_ptr<Class>(Args...)>
    (static_cast<fn_type>
     (&function_for_constructor_helper<Class, Args...>::construct_shared_ptr));
}

	
inline std::string class_name(type_info const& info)
{
	return "v8pp::class_<" + info.name() + '>';
}

inline std::string pointer_str(void const* ptr)
{
	std::string buf(sizeof(void*) * 2 + 3, 0); // +3 for 0x and \0 terminator
	int const len = 
#if defined(_MSC_VER) && (_MSC_VER < 1900)
		sprintf_s(&buf[0], buf.size(), "%p", ptr);
#else
		snprintf(&buf[0], buf.size(), "%p", ptr);
#endif
	buf.resize(len < 0? 0 : len);
	return buf;
}

class class_info
{
public:
	explicit class_info(type_info const& type) : type_(type) {}
	class_info(class_info const&) = delete;
	class_info& operator=(class_info const&) = delete;
	virtual ~class_info() = default;

	type_info const& type() const { return type_; }

	using cast_function = void* (*)(void* ptr);

	// Manages a void* which is actually a shared_ptr<T>*.
	// Fortunately, the destruction function saved in it knows what T is,
	// so the shared_ptr<T>* is deleted appropriately when the count
	// goes to 0.
	typedef std::shared_ptr<void> managed_shared_ptr_ptr;

	using managed_shared_ptr_ptr_cast_function =
		managed_shared_ptr_ptr (*) (const managed_shared_ptr_ptr&);
	
	void add_base(class_info* info, cast_function cast, managed_shared_ptr_ptr_cast_function spcast)
	{
		auto it = std::find_if(bases_.begin(), bases_.end(),
			[info](base_class_info const& base) { return base.info == info; });
		if (it != bases_.end())
		{
			//assert(false && "duplicated inheritance");
			throw std::runtime_error(class_name(type_)
				+ " is already inherited from " + class_name(info->type_));
		}
		bases_.emplace_back(info, cast, spcast);
		info->derivatives_.emplace_back(this);
	}

	bool cast(void*& ptr, type_info const& type) const
	{
		if (type == type_ || !ptr)
		{
			return true;
		}

		// fast way - search a direct parent
		for (base_class_info const& base : bases_)
		{
			if (base.info->type_ == type)
			{
				ptr = base.cast(ptr);
				return true;
			}
		}

		// slower way - walk on hierarhy
		for (base_class_info const& base : bases_)
		{
			void* p = base.cast(ptr);
			if (base.info->cast(p, type))
			{
				ptr = p;
				return true;
			}
		}

		return false;
	}

	managed_shared_ptr_ptr managed_shared_ptr_ptr_cast
	(managed_shared_ptr_ptr ptr, type_info const& type) const
	{
		if (type == type_ || !ptr) { return ptr; }
		for (base_class_info const& base: bases_)
		{
			managed_shared_ptr_ptr base_ptr = base.managed_shared_ptr_ptr_cast(ptr);
			assert(base_ptr != nullptr);
			managed_shared_ptr_ptr result = base.info->managed_shared_ptr_ptr_cast(base_ptr, type);
			if (result != nullptr) { return result; }
		}
		return nullptr;
	}
	
#if 0
	template<typename T>
	void add_object(T* object, persistent<v8::Object>&& handle)
	{
		auto it = objects_.find(object);
		if (it != objects_.end())
		{
			//assert(false && "duplicate object");
			throw std::runtime_error(class_name(type())
				+ " duplicate object " + pointer_str(object));
		}
		objects_.emplace(object, std::move(handle));
	}
#endif

	template <typename T>
	void add_object(v8::Isolate* isolate,
									T* object,
									persistent<v8::Object>&& handle,
									bool claim_ownership,
									bool count_against_vm_size,
									const std::function<size_t(const T*)>& obj_size_func) {
		auto it = object_records_.find(object);
		if (it != object_records_.end())
		{
			//assert(false && "duplicate object");
			throw std::runtime_error(class_name(type())
				+ " duplicate object " + pointer_str(object));
		}
		object_record orec(std::move(handle), claim_ownership,
											 count_against_vm_size);
		object_records_.emplace(object, std::move(orec));
		if (count_against_vm_size) {
			size_t sz = obj_size_func(object);
			isolate->AdjustAmountOfExternalAllocatedMemory(static_cast<int64_t>(sz));
		}
	}
	
	template <typename T>
	void add_shared_object
	(v8::Isolate* isolate,
	 std::shared_ptr<T> object,
	 persistent<v8::Object>&& handle,
	 bool count_against_vm_size,
	 const std::function<size_t(const T*)>& obj_size_func)
	{
		auto it = object_records_.find(object.get());
		if (it != object_records_.end())
		{
			throw std::runtime_error(class_name(type())
															 + " duplicate object " + pointer_str(object.get()));
		}

		managed_shared_ptr_ptr mspp(new std::shared_ptr<T>(object));
		object_record orec(std::move(handle), std::move(mspp),
											 count_against_vm_size);
		object_records_.emplace(object.get(), std::move(orec));
		if (count_against_vm_size) {
			size_t sz = obj_size_func(object.get());
			isolate->AdjustAmountOfExternalAllocatedMemory(static_cast<int64_t>(sz));
		}
	}

	bool has_shared_ptr_for_object(void* obj) {
		auto it = object_records_.find(obj);
		if (it == object_records_.end()) { return false; }
		return it->second.has_shared_ptr();
	}

#if 0	
	template<typename T>
	void remove_object(v8::Isolate* isolate, T* object, void (*destroy)(v8::Isolate* isolate, T* obj) = nullptr)
	{
		auto it = objects_.find(object);
		assert(objects_.find(object) != objects_.end() && "no object");
		if (it != objects_.end())
		{
			if (!it->second.IsNearDeath())
			{
				// remove pointer to wrapped  C++ object from V8 Object internal field
				// to disable unwrapping for this V8 Object
				assert(to_local(isolate, it->second)->GetAlignedPointerFromInternalField(0) == object);
				to_local(isolate, it->second)->SetAlignedPointerInInternalField(0, nullptr);
			}
			it->second.Reset();
			bool has_shared_ptr = has_shared_ptr_for_object(object);
			if (destroy && !has_shared_ptr)
			{
				destroy(isolate, object);
			}
			objects_.erase(it);
			if (has_shared_ptr) {
				shared_ptrs_.erase(object);
			}
		}
	}
#endif

	template <typename T>
	void remove_object(v8::Isolate* isolate,
										 T* object,
										 const std::function<void(T*)>& destroy_func,
										 const std::function<size_t(const T*)> obj_size_func) 
	{
		auto it = object_records_.find(object);
		assert(it != object_records_.end() && "no object");
		if (it != object_records_.end())
		{
			if (!it->second.v8object.IsNearDeath())
			{
				// remove pointer to wrapped  C++ object from V8 Object internal field
				// to disable unwrapping for this V8 Object
				assert(to_local(isolate, it->second.v8object)->
							 GetAlignedPointerFromInternalField(0) == object);
				to_local(isolate, it->second.v8object)->
					SetAlignedPointerInInternalField(0, nullptr);
			}
			it->second.v8object.Reset();
			if (it->second.count_against_vm_size && (obj_size_func != nullptr))
			{
				size_t sz = obj_size_func(object);
				isolate->AdjustAmountOfExternalAllocatedMemory
					(-static_cast<int64_t>(sz));
			}
			bool has_shared_ptr = it->second.has_shared_ptr();
			if (!has_shared_ptr && (destroy_func != nullptr)) {
				destroy_func(object);
			}
			object_records_.erase(object);
		}
	}

#if 0
	template<typename T>
	void remove_objects(v8::Isolate* isolate, void (*destroy)(v8::Isolate* isolate, T* obj))
	{
		for (auto& object_rec : objects_)
		{
			bool has_shared_ptr = has_shared_ptr_for_object(object_rec.first);
			object_rec.second.Reset();
			if (destroy && !has_shared_ptr) 
			{
				destroy(isolate, static_cast<T*>(object_rec.first));
			}
		}
		objects_.clear();
		shared_ptrs_.clear();
	}
#endif

	template<typename T>
	void remove_objects(v8::Isolate* isolate,
											const std::function<void(T*)>& destroy_func,
											const std::function<size_t(const T*)>& obj_size_func) {
		for (auto& object_rec: object_records_)
		{
			bool has_shared_ptr = object_rec.second.has_shared_ptr();
			object_rec.second.v8object.Reset();
			T* obj = static_cast<T*>(object_rec.first);			
			if (object_rec.second.count_against_vm_size &&
					(obj_size_func != nullptr))
			{
				size_t sz = obj_size_func(obj);
				isolate->AdjustAmountOfExternalAllocatedMemory
					(-static_cast<int64_t>(sz));
			}
			if (!has_shared_ptr && (destroy_func != nullptr))
			{
				destroy_func(obj);
			}
		}
		
	}

	v8::Local<v8::Object> find_object(v8::Isolate* isolate, void const* object) const
	{
		auto it = object_records_.find(const_cast<void*>(object));
		if (it != object_records_.end())
		{
			return to_local(isolate, it->second.v8object);
		}

		v8::Local<v8::Object> result;
		for (class_info const* info : derivatives_)
		{
			result = info->find_object(isolate, object);
			if (!result.IsEmpty()) break;
		}
		return result;
	}

	managed_shared_ptr_ptr find_managed_shared_ptr_ptr(void const* object) const
	{
		auto it = object_records_.find(const_cast<void*>(object));
		if (it != object_records_.end())
  	{
			return it->second.shptr;
		}
		for (class_info const* info: derivatives_)
		{
			auto result = info->find_managed_shared_ptr_ptr(object);
			if (result != nullptr) { return result; }
		}
		return nullptr;
	}
	
private:
	struct base_class_info
	{
		class_info* info;
		cast_function cast;
		managed_shared_ptr_ptr_cast_function managed_shared_ptr_ptr_cast;

		base_class_info(class_info* info, cast_function cast, managed_shared_ptr_ptr_cast_function spcast)
			: info(info)
			, cast(cast)
			, managed_shared_ptr_ptr_cast(spcast)
		{
		}
	};

	type_info const type_;
	std::vector<base_class_info> bases_;
	std::vector<class_info*> derivatives_;

	struct object_record
	{
		persistent<v8::Object> v8object;
		managed_shared_ptr_ptr shptr;
		bool destroy: 1;
		bool count_against_vm_size: 1;

		object_record(persistent<v8::Object>&& v8o, bool desty,
									bool count_against_vms):
			v8object(std::move(v8o)),
			destroy(desty),
			count_against_vm_size(count_against_vms)
		{;}

		object_record(persistent<v8::Object>&& v8o,
									managed_shared_ptr_ptr&& shp,
									bool count_against_vm_sz):
			v8object(std::move(v8o)),
			shptr(std::move(shp)),
			destroy(false),
			count_against_vm_size(count_against_vm_sz)
		{;}

		bool has_shared_ptr() const { return shptr != nullptr; }
		
	};
	
	//std::unordered_map<void*, persistent<v8::Object>> objects_;
	//std::unordered_map<void*, managed_shared_ptr_ptr> shared_ptrs_;
		std::unordered_map<void*, object_record> object_records_;
	
};

template<typename T>
class class_singleton;

class class_singletons
{
public:
	template<typename T>
	static class_singleton<T>& add_class(v8::Isolate* isolate)
	{
		class_singletons* singletons = instance(add, isolate);
		type_info const type = type_id<T>();
		auto it = singletons->find(type);
		if (it != singletons->classes_.end())
		{
			//assert(false && "class already registred");
			throw std::runtime_error(class_name(type)
				+ " is already exist in isolate " + pointer_str(isolate));
		}
		singletons->classes_.emplace_back(new class_singleton<T>(isolate, type));
		return *static_cast<class_singleton<T>*>(singletons->classes_.back().get());
	}

	template<typename T>
	static void remove_class(v8::Isolate* isolate)
	{
		class_singletons* singletons = instance(get, isolate);
		if (singletons)
		{
			type_info const type = type_id<T>();
			auto it = singletons->find(type);
			if (it != singletons->classes_.end())
			{
				singletons->classes_.erase(it);
				if (singletons->classes_.empty())
				{
					instance(remove, isolate);
				}
			}
		}
	}

	template<typename T>
	static class_singleton<T>& find_class(v8::Isolate* isolate)
	{
		class_singletons* singletons = instance(get, isolate);
		type_info const type = type_id<T>();
		if (singletons)
		{
			auto it = singletons->find(type);
			if (it != singletons->classes_.end())
			{
				return *static_cast<class_singleton<T>*>(it->get());
			}
		}
		//assert(false && "class not registered");
		throw std::runtime_error(class_name(type)
			+ " not found in isolate " + pointer_str(isolate));
	}

	static void remove_all(v8::Isolate* isolate)
	{
		instance(remove, isolate);
	}

private:
	using classes = std::vector<std::unique_ptr<class_info>>;
	classes classes_;

	classes::iterator find(type_info const& type)
	{
		return std::find_if(classes_.begin(), classes_.end(),
			[&type](std::unique_ptr<class_info> const& info) { return info->type() == type; });
	}

	enum operation { get, add, remove };
	static class_singletons* instance(operation op, v8::Isolate* isolate)
	{
#if defined(V8PP_ISOLATE_DATA_SLOT)
		class_singletons* instances =
			static_cast<class_singletons*>(isolate->GetData(V8PP_ISOLATE_DATA_SLOT));
		switch (op)
		{
		case get:
			return instances;
		case add:
			if (!instances)
			{
				instances = new class_singletons;
				isolate->SetData(V8PP_ISOLATE_DATA_SLOT, instances);
			}
			return instances;
		case remove:
			if (instances)
			{
				delete instances;
				isolate->SetData(V8PP_ISOLATE_DATA_SLOT, nullptr);
			}
		default:
			return nullptr;
		}
#else
		static std::unordered_map<v8::Isolate*, class_singletons> instances;
		switch (op)
		{
		case get:
			{
				auto it = instances.find(isolate);
				return it != instances.end()? &it->second : nullptr;
			}
		case add:
			return &instances[isolate];
		case remove:
			instances.erase(isolate);
		default:
			return nullptr;
		}
#endif
	}
};

template<typename T>
size_t default_object_size_func(const T* t) {
	return sizeof(T);
}

template<typename T>
void default_delete_func(T* t) { delete t; }

template<typename T>
class class_singleton : public class_info
{
public:
	class_singleton(v8::Isolate* isolate, type_info const& type)
		: class_info(type)
		, isolate_(isolate)
		, ctor_(nullptr)
		, shared_ctor_(nullptr)
		, dtor_(&default_delete_func<T>)
		, object_size_func_(&default_object_size_func<T>)
		, count_shared_as_externally_allocated_(false)
		, automatically_wrap_missing_objects_as_external_(false)
			
	{
		v8::Local<v8::FunctionTemplate> func = v8::FunctionTemplate::New(isolate_);
		v8::Local<v8::FunctionTemplate> js_func = v8::FunctionTemplate::New(isolate_,
			[](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			v8::Isolate* isolate = args.GetIsolate();
			try
			{
				return args.GetReturnValue().Set(class_singletons::find_class<T>
																				 (isolate).wrap_object(args));
			}
			catch (std::exception const& ex)
			{
				args.GetReturnValue().Set(throw_ex(isolate, ex.what()));
			}
		});

		func_.Reset(isolate_, func);
		js_func_.Reset(isolate_, js_func);

		// each JavaScript instance has 2 internal fields:
		//  0 - pointer to a wrapped C++ object
		//  1 - pointer to the class_singleton
		func->InstanceTemplate()->SetInternalFieldCount(2);
	}

	class_singleton(class_singleton const&) = delete;
	class_singleton& operator=(class_singleton const&) = delete;

	~class_singleton()
	{
		remove_objects();
	}

	v8::Handle<v8::Object> wrap(T* object, bool claim_ownership,
															bool count_against_vm_size)
	{
		v8::EscapableHandleScope scope(isolate_);

		v8::Local<v8::Object> obj =
			class_function_template()->GetFunction()->NewInstance();
		obj->SetAlignedPointerInInternalField(0, object);
		obj->SetAlignedPointerInInternalField(1, this);

		persistent<v8::Object> pobj(isolate_, obj);

		pobj.SetWeak
			(object,
			 [](v8::WeakCallbackInfo<T> const& data)
			 {
				 v8::Isolate* isolate = data.GetIsolate();
				 T* object = data.GetParameter();
				 auto& csing = class_singletons::find_class<T>(isolate);
				 csing.remove_object(object);
			 }
			 , v8::WeakCallbackType::kParameter
			 );

		class_info::add_object(isolate_, object, std::move(pobj),
													 claim_ownership,
													 count_against_vm_size,
													 object_size_func_);

		return scope.Escape(obj);
	}

	v8::Handle<v8::Object> wrap_shared(std::shared_ptr<T> object,
																		 bool count_against_vm_size)
	{
		v8::EscapableHandleScope scope(isolate_);

		v8::Local<v8::Object> obj = class_function_template()->GetFunction()->NewInstance();
		obj->SetAlignedPointerInInternalField(0, object.get());
		obj->SetAlignedPointerInInternalField(1, this);

		persistent<v8::Object> pobj(isolate_, obj);

		auto callback =
			[](v8::WeakCallbackInfo<T> const& data)
			{
				v8::Isolate* isolate = data.GetIsolate();
				T* object = data.GetParameter();
				auto& csing = class_singletons::find_class<T>(isolate);
				csing.remove_object(object);
			};
		
		pobj.SetWeak
			(object.get(), callback
			 , v8::WeakCallbackType::kParameter
			 );
			 
		class_info::add_shared_object(isolate_, object, std::move(pobj),
																	count_against_vm_size,
																	object_size_func_);
		return scope.Escape(obj);
	}
	
	
	v8::Isolate* isolate() { return isolate_; }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return to_local(isolate_, func_);
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return to_local(isolate_, js_func_.IsEmpty()? func_ : js_func_);
	}

#if 0
	template<typename ...Args>
	void ctor()
	{
		ctor_ = [](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			using ctor_type = T* (*)(v8::Isolate* isolate, Args...);
			return call_from_v8(static_cast<ctor_type>(&factory<T>::create), args);
		};
		shared_ctor_ = [](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			using ctor_type = std::shared_ptr<T> (*)(Args...);
			return call_from_v8(static_cast<ctor_type>(&shared_object_factory<T>::create), args);
		};
		class_function_template()->Inherit(js_function_template());
	}
#endif

	// Uses T constructor with given type signature 
	template <typename ...Args>
	void use_class_constructor() {
		assert(ctor_ == nullptr);
		assert(shared_ctor_ == nullptr);		
		auto fn = get_function_for_constructor<T, Args...>();
		ctor_ = [fn](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			return call_from_v8(fn, args);
		};
		class_function_template()->Inherit(js_function_template());		
	}

	// Uses T constructor with given type signature, but wraps the result
	// in a shared_ptr<T>
	template <typename ...Args>
	void use_class_constructor_with_shared_ptr() {
		assert(ctor_ == nullptr);
		assert(shared_ctor_ == nullptr);		
		auto fn = get_function_for_shared_ptr_from_constructor<T, Args...>();
		shared_ctor_ = [fn](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			return call_from_v8(fn, args);
		};
		class_function_template()->Inherit(js_function_template());		
	}

	// Calls given function to construct an object. Function must return a T*.
	template <typename F>
	void use_function_as_constructor(F fn) {
		assert(ctor_ == nullptr);
		assert(shared_ctor_ == nullptr);		
		ctor_ = [fn](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			return call_from_v8(fn, args);
		};
		class_function_template()->Inherit(js_function_template());		
	}

	// Calls a function to construct an object. Function must return a
	// shared_ptr<T>.
	template <typename F>
	void use_function_as_constructor_with_shared_ptr(F fn) {
		assert(ctor_ == nullptr);
		assert(shared_ctor_ == nullptr);
		shared_ctor_ = [fn](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			return call_from_v8(fn, args);
		};
		class_function_template()->Inherit(js_function_template());		
	}

	void set_destroy_func(const std::function<void(T*)>& f) {
		dtor_ = f;
	}

	const std::function<void(T*)>& get_destroy_func() const { return dtor_; }
	
	void set_object_size_func(const std::function<size_t(const T*)>& f) {
		object_size_func_ = f;
	}

	const std::function<size_t(const T*)>& get_object_size_func() const
	{
		return object_size_func_;
	}

	void set_count_shared_against_vm_size(bool c)
	{
		count_shared_as_externally_allocated_ = c;
	}

	void set_automatically_wrap_missing_objects_as_external(bool a) {
		automatically_wrap_missing_objects_as_external_ = a;
	}
	
	bool get_count_shared_against_vm_size() const
	{
		return count_shared_as_externally_allocated_;
	}
	
	template<typename U>
	void inherit()
	{
		class_singleton<U>* base = &class_singletons::find_class<U>(isolate_);
		add_base(base,
			[](void* ptr) -> void*
			{
				return static_cast<U*>(static_cast<T*>(ptr));
			},
			[](const managed_shared_ptr_ptr& p) -> managed_shared_ptr_ptr
			{
				std::shared_ptr<T>* tp = static_cast<std::shared_ptr<T>*>(p.get());
				std::shared_ptr<U>* up = new std::shared_ptr<U>(*tp);
				return managed_shared_ptr_ptr(up);
			});
		js_function_template()->Inherit(base->class_function_template());
	}

	v8::Handle<v8::Object> wrap_external_object(T* object)
	{
		return wrap(object, false, false);
	}

	v8::Handle<v8::Object> wrap_object(T* object)
	{
		return wrap(object, true, true);
	}

	v8::Handle<v8::Object> wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args)
	{

		if (ctor_) {
			return wrap_object(ctor_(args));
		} else if (shared_ctor_) {
			return wrap_shared(shared_ctor_(args),
												 count_shared_as_externally_allocated_);
		} else {
			throw std::runtime_error(class_name(type()) + " has no constructor");
		}
#if 0
		if (!ctor_)
		{
			//assert(false && "create not allowed");
			throw std::runtime_error(class_name(type()) + " has no constructor");
		}
		if (construct_using_shared_ptr_)
		{
			return wrap_shared(shared_ctor_(args));
		}
		else
		{
			return wrap_object(ctor_(args));
		}
#endif
	}

	T* unwrap_object(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);

		while (value->IsObject())
		{
			v8::Handle<v8::Object> obj = value->ToObject();
			if (obj->InternalFieldCount() == 2)
			{
				void* ptr = obj->GetAlignedPointerFromInternalField(0);
				class_info* info = static_cast<class_info*>(obj->GetAlignedPointerFromInternalField(1));
				if (info && info->cast(ptr, type()))
				{
					return static_cast<T*>(ptr);
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
	}

	std::shared_ptr<T> unwrap_shared_object(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);
		while (value->IsObject())
  	{
			v8::Handle<v8::Object> obj = value->ToObject();
			if (obj->InternalFieldCount() == 2)
			{
				void* ptr = obj->GetAlignedPointerFromInternalField(0);
				class_info* info = static_cast<class_info*>(obj->GetAlignedPointerFromInternalField(1));
				if (info)
				{
					managed_shared_ptr_ptr mspp(info->find_managed_shared_ptr_ptr(ptr));
					if (mspp)
					{
						managed_shared_ptr_ptr cast_mspp = info->managed_shared_ptr_ptr_cast
							(mspp, type());
						if (cast_mspp != nullptr)
						{
							return *(static_cast<std::shared_ptr<T>*>(cast_mspp.get()));
						}
					}
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
	}
	
	v8::Handle<v8::Object> find_object(T const* obj)
	{
		auto result = class_info::find_object(isolate_, obj);
		if (result.IsEmpty() && automatically_wrap_missing_objects_as_external_) {
			// XXX this is dodgy
			return wrap_external_object(const_cast<T*>(obj));
		}
		return result;
	}

#if 0
	void destroy_objects()
	{
		class_info::remove_objects(isolate_, &factory<T>::destroy);
	}

	void destroy_object(T* obj)
	{
		class_info::remove_object(isolate_, obj, &factory<T>::destroy);
	}
#endif

	void remove_object(T* obj)
	{
		class_info::remove_object(isolate_, obj, dtor_, object_size_func_);
	}
	
	void remove_objects()
	{
		class_info::remove_objects(isolate_, dtor_, object_size_func_);
	}

private:
	v8::Isolate* isolate_;
	std::function<T* (v8::FunctionCallbackInfo<v8::Value> const& args)> ctor_;
	std::function<std::shared_ptr<T> (v8::FunctionCallbackInfo<v8::Value> const& args)> shared_ctor_;
	std::function<void(T*)> dtor_;
	typedef std::function<size_t(const T*)> object_size_func_type;
	object_size_func_type object_size_func_;
	bool count_shared_as_externally_allocated_;
	bool automatically_wrap_missing_objects_as_external_;
	
	v8::UniquePersistent<v8::FunctionTemplate> func_;
	v8::UniquePersistent<v8::FunctionTemplate> js_func_;
};

} // namespace detail

struct class_construct_using_shared_ptr_tag {};

/// Interface for registering C++ classes in V8
template<typename T>
class class_
{
	using class_singleton = detail::class_singleton<T>;
	detail::class_singleton<T>& class_singleton_;
public:
	explicit class_(v8::Isolate* isolate)
		: class_singleton_(detail::class_singletons::add_class<T>(isolate))
	{
	}

#if 0
	class_(v8::Isolate* isolate, class_construct_using_shared_ptr_tag)
		: class_singleton_(detail::class_singletons::add_class<T>(isolate, true))
	{
	}
#endif
	
#if 0
	/// Set class constructor signature
	template<typename ...Args>
	class_& ctor()
	{
		class_singleton_.template ctor<Args...>();
		return *this;
	}
#endif

	template<typename ...Args>
	class_& use_class_constructor()
	{
		class_singleton_.template use_class_constructor<Args...>();
		return *this;
	}

	template<typename ...Args>
	class_& use_class_constructor_with_shared_ptr()
	{
		class_singleton_.template use_class_constructor_with_shared_ptr<Args...>();
		return *this;
	}
	
	template<typename F>
	class_& use_function_as_constructor(F&& fn) {
		class_singleton_.template use_function_as_constructor(std::forward<F>(fn));
		return *this;
	}

	template<typename F>
	class_& use_function_as_constructor_with_shared_ptr(F&& fn) {
		class_singleton_.template use_function_as_constructor_with_shared_ptr
			(std::forward<F>(fn));
		return *this;
	}

	class_& set_destroy_func(const std::function<void(T*)>& f) {
		class_singleton_.set_destroy_func(f);
		return *this;
	}

	class_& set_object_size_func(const std::function<size_t(const T*)>& f) {
		class_singleton_.set_object_size_func(f);
		return *this;
	}

	class_& set_count_shared_against_vm_size(bool c) {
		class_singleton_.set_count_shared_against_vm_size(c);
		return *this;
	}

	class_& set_automatically_wrap_missing_objects_as_external(bool s) {
		class_singleton_.set_automatically_wrap_missing_objects_as_external(s);
		return *this;
	}
	
	/// Inhert from C++ class U
	template<typename U>
	class_& inherit()
	{
		static_assert(std::is_base_of<U, T>::value, "Class U should be base for class T");
		//TODO: std::is_convertible<T*, U*> and check for duplicates in hierarchy?
		class_singleton_.template inherit<U>();
		return *this;
	}

	/// Set C++ class member function
	template<typename Method>
	typename std::enable_if<
		std::is_member_function_pointer<Method>::value, class_&>::type
	set(char const *name, Method mem_func)
	{
		class_singleton_.class_function_template()->PrototypeTemplate()->Set(
			isolate(), name, wrap_function_template(isolate(), mem_func));
		return *this;
	}

	/// Set static class function
	template<typename Function, typename Fun = typename std::decay<Function>::type>
	typename std::enable_if<detail::is_callable<Fun>::value, class_&>::type
	set(char const *name, Function&& func)
	{
		class_singleton_.js_function_template()->Set(isolate(), name,
			wrap_function_template(isolate(), std::forward<Fun>(func)));
		return *this;
	}

	/// Set class member function even if it's not a C++ member function
	template <typename Function, typename Fun = typename std::decay<Function>::type>
	class_& set_object_member_function(char const *name, Function&& func)
	{
		class_singleton_.class_function_template()->PrototypeTemplate()->Set(
			isolate(), name, wrap_function_template_called_as_method
			(isolate(), std::forward<Fun>(func)));
		return *this;
	}

	/// Set static class function
	template <typename Function,
						typename Fun = typename std::decay<Function>::type,
						typename std::enable_if
						<!std::is_member_function_pointer<Fun>::value,
						 int>::type=0>
	class_& set_static_class_function(char const* name, Function&& func)
	{
		class_singleton_.js_function_template()->Set(isolate(), name,
			wrap_function_template_called_as_nonmethod
			(isolate(), std::forward<Fun>(func)));
		return *this;
	}

	/// Set class member data
	template<typename Attribute>
	typename std::enable_if<
		std::is_member_object_pointer<Attribute>::value, class_&>::type
	set(char const *name, Attribute attribute, bool readonly = false)
	{
		v8::HandleScope scope(isolate());

		v8::AccessorGetterCallback getter = &member_get<Attribute>;
		v8::AccessorSetterCallback setter = &member_set<Attribute>;
		if (readonly)
		{
			setter = nullptr;
		}

		v8::Handle<v8::Value> data = detail::set_external_data(isolate(), std::forward<Attribute>(attribute));
		v8::PropertyAttribute const prop_attrs = v8::PropertyAttribute(v8::DontDelete | (setter? 0 : v8::ReadOnly));

		class_singleton_.class_function_template()->PrototypeTemplate()->SetAccessor(
			v8pp::to_v8(isolate(), name), getter, setter, data, v8::DEFAULT, prop_attrs);
		return *this;
	}

	/// Set class attribute with getter and setter
	template<typename GetMethod, typename SetMethod>
	typename std::enable_if<std::is_member_function_pointer<GetMethod>::value
		&& std::is_member_function_pointer<SetMethod>::value, class_&>::type
	set(char const *name, property_<GetMethod, SetMethod>&& prop)
	{
		using prop_type = property_<GetMethod, SetMethod>;
		v8::HandleScope scope(isolate());

		v8::AccessorGetterCallback getter = prop_type::get;
		v8::AccessorSetterCallback setter = prop_type::set;
		if (prop_type::is_readonly)
		{
			setter = nullptr;
		}

		v8::Handle<v8::Value> data = detail::set_external_data(isolate(), std::forward<prop_type>(prop));
		v8::PropertyAttribute const prop_attrs = v8::PropertyAttribute(v8::DontDelete | (setter? 0 : v8::ReadOnly));

		class_singleton_.class_function_template()->PrototypeTemplate()->SetAccessor(v8pp::to_v8(isolate(), name),
			getter, setter, data, v8::DEFAULT, prop_attrs);
		return *this;
	}

	/// Set value as a read-only property
	template<typename Value>
	class_& set_const(char const* name, Value const& value)
	{
		v8::HandleScope scope(isolate());

		class_singleton_.class_function_template()->PrototypeTemplate()->Set(v8pp::to_v8(isolate(), name),
			to_v8(isolate(), value), v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete));
		return *this;
	}

	/// v8::Isolate where the class bindings belongs
	v8::Isolate* isolate() { return class_singleton_.isolate(); }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return class_singleton_.class_function_template();
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return class_singleton_.js_function_template();
	}

	/// Create JavaScript object which references externally created C++ class.
	/// It will not take ownership of the C++ pointer.
	static v8::Handle<v8::Object> reference_external(v8::Isolate* isolate, T* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).wrap_external_object(ext);
	}

	static void remove_object(v8::Isolate* isolate, T* obj)
	{
		detail::class_singletons::find_class<T>(isolate).remove_object(obj);
	}
	
#if 0
	/// Remove external reference from JavaScript
	static void unreference_external(v8::Isolate* isolate, T* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).remove_object(isolate, ext);
	}
#endif
	
	/// As reference_external but delete memory for C++ object
	/// when JavaScript object is deleted. You must use "new" to allocate ext.
	static v8::Handle<v8::Object> import_external(v8::Isolate* isolate, T* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).wrap_object(ext);
	}

	/// Get wrapped object from V8 value, may return nullptr on fail.
	static T* unwrap_object(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return detail::class_singletons::find_class<T>(isolate).unwrap_object(value);
	}

	static std::shared_ptr<T> unwrap_shared_object(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return detail::class_singletons::find_class<T>(isolate).unwrap_shared_object(value);
	}
	
	static v8::Handle<v8::Object> wrap_shared_object(v8::Isolate* isolate, std::shared_ptr<T> obj)
	{
		auto& c = detail::class_singletons::find_class<T>(isolate);
		return c.wrap_shared
			(obj, c.get_count_shared_against_vm_size());
	}

#if 0
  // Removed. Use wrap_[shared_]object instead after constructing it yourself.
	
	/// Create a wrapped C++ object and import it into JavaScript.
	template<typename ...Args>
	static v8::Handle<v8::Object> create_object(v8::Isolate* isolate, Args... args)
	{
		T* obj = v8pp::factory<T>::create(isolate, std::forward<Args>(args)...);
		return import_external(isolate, obj);
	}

	template <typename ...Args>
	static v8::Handle<v8::Object> create_shared_object
	(v8::Isolate* isolate, std::shared_ptr<T>* shared_ptr_ptr,  Args... args) {
		T* obj = v8pp::factory<T>::create(isolate, std::forward<Args>(args)...);
		std::shared_ptr<T> sp(obj);
		if (shared_ptr_ptr != nullptr) { *shared_ptr_ptr = sp; }
		return wrap_shared_object(isolate, sp);
	}
#endif
	
	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail.
	static v8::Handle<v8::Object> find_object(v8::Isolate* isolate, T const* obj)
	{
		return detail::class_singletons::find_class<T>(isolate).find_object(obj);
	}

	static v8::Handle<v8::Object> find_object(v8::Isolate* isolate,
																						std::shared_ptr<T> optr) {
		return find_object(optr.get());
	}
	
#if 0
	/// Destroy wrapped C++ object
	static void destroy_object(v8::Isolate* isolate, T* obj)
	{
		detail::class_singletons::find_class<T>(isolate).destroy_object(obj);
	}

	/// Destroy all wrapped C++ objects of this class
	static void destroy_objects(v8::Isolate* isolate)
	{
		detail::class_singletons::find_class<T>(isolate).destroy_objects();
	}

	/// Destroy all wrapped C++ objects and this binding class_
	static void destroy(v8::Isolate* isolate)
	{
		detail::class_singletons::remove_class<T>(isolate);
	}
#endif

	/// Remove all wrapped C++ objects of this class
	static void remove_objects(v8::Isolate* isolate)
	{
		detail::class_singletons::find_class<T>(isolate).remove_objects();
	}

	/// Destroy all wrapped C++ objects and this binding class_
	static void remove(v8::Isolate* isolate)
	{
		detail::class_singletons::remove_class<T>(isolate);
	}

	// TEMPORARY STUFF we're going to delete once we're done refactoring and
	// debugging

	static void destroy(v8::Isolate* isolate)
	{
		remove(isolate);
	}
	
	template <typename ...Args>
	class_& ctor()
	{
		return use_class_constructor<Args...>();
	}
	
private:
	template<typename Attribute>
	static void member_get(v8::Local<v8::String>, v8::PropertyCallbackInfo<v8::Value> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		T const& self = v8pp::from_v8<T const&>(isolate, info.This());
		Attribute attr = detail::get_external_data<Attribute>(info.Data());
		info.GetReturnValue().Set(to_v8(isolate, self.*attr));
	}

	template<typename Attribute>
	static void member_set(v8::Local<v8::String>, v8::Local<v8::Value> value, v8::PropertyCallbackInfo<void> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		T& self = v8pp::from_v8<T&>(isolate, info.This());
		Attribute ptr = detail::get_external_data<Attribute>(info.Data());
		using attr_type = typename detail::function_traits<Attribute>::return_type;
		self.*ptr = v8pp::from_v8<attr_type>(isolate, value);
	}
};

inline void cleanup(v8::Isolate* isolate)
{
	detail::class_singletons::remove_all(isolate);
}

} // namespace v8pp

#endif // V8PP_CLASS_HPP_INCLUDED
