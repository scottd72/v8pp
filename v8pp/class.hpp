//
// Copyright (c) 2013-2016 Pavel Medvedev. All rights reserved.
//
// This file is part of v8pp (https://github.com/pmed/v8pp) project.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Extensive modifications and additions written by Scott Davies (distributed
// under the same license).

#ifndef V8PP_CLASS_HPP_INCLUDED
#define V8PP_CLASS_HPP_INCLUDED

#include <cstdio>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

	using cast_function = void const* (*)(void const* ptr);

	// Manages a void* which is actually a shared_ptr<T>*.
	// Fortunately, the destruction function saved in it knows what T is,
	// so the shared_ptr<T>* is deleted appropriately when the count
	// goes to 0.
	typedef std::shared_ptr<void> managed_shared_ptr_ptr;

	using managed_shared_ptr_ptr_cast_function =
		managed_shared_ptr_ptr (*) (const managed_shared_ptr_ptr&);
	
	struct object_record
	{
		persistent<v8::Object> v8object;
		managed_shared_ptr_ptr shptr;
		bool can_modify: 1;
		bool destroy: 1;
		bool count_against_vm_size: 1;

		object_record(persistent<v8::Object>&& v8o,
									bool can_mod,
									bool desty,
									bool count_against_vms):
			v8object(std::move(v8o)),
			can_modify(can_mod),
			destroy(desty),
			count_against_vm_size(count_against_vms)
		{;}

		object_record(persistent<v8::Object>&& v8o,
									managed_shared_ptr_ptr&& shp,
									bool can_mod,
									bool count_against_vm_sz):
			v8object(std::move(v8o)),
			shptr(std::move(shp)),
			can_modify(can_mod), 
			destroy(false),
			count_against_vm_size(count_against_vm_sz)
		{;}

		bool has_shared_ptr() const { return shptr != nullptr; }
		
	};
	
	explicit class_info(type_info const& type) : type_(type) {}
	class_info(class_info const&) = delete;
	class_info& operator=(class_info const&) = delete;
	virtual ~class_info() = default;

	type_info const& type() const { return type_; }

	void add_base(class_info* info,
								cast_function ucast,
								cast_function dcast,
								managed_shared_ptr_ptr_cast_function spucast)
	{
		auto it = std::find_if(bases_.begin(), bases_.end(),
			[info](base_class_info const& base) { return base.info == info; });
		if (it != bases_.end())
		{
			//assert(false && "duplicated inheritance");
			throw std::runtime_error(class_name(type_)
				+ " is already inherited from " + class_name(info->type_));
		}
		bases_.emplace_back(info, ucast, spucast);
		info->derivatives_.emplace_back(this, dcast);
	}

	// ptr is assumed to be a pointer to an object of "our" type.
	// We will try to cast it to a pointer to a base class of the given type.
	bool upcast(void const*& ptr, type_info const& type) const
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
				ptr = base.upcast(ptr);
				return true;
			}
		}

		// slower way - walk on hierarhy
		for (base_class_info const& base : bases_)
		{
			// Manufacture a pointer of type Base*
			void const* p = base.upcast(ptr); 

			// See if we can translate that into a pointer of the target
			// even-more-basic type
			if (base.info->upcast(p, type))
			{
				ptr = p;
				return true;
			}
		}

		return false;
	}

	bool upcast(void*& ptr, type_info const& type) const {
		void const* cptr(ptr);
		if (upcast(cptr, type)) {
			ptr = const_cast<void*>(cptr);
			return true;
		}
		return false;
	}
	
	managed_shared_ptr_ptr managed_shared_ptr_ptr_upcast
	(managed_shared_ptr_ptr ptr, type_info const& type) const
	{
		if (type == type_ || !ptr) { return ptr; }
		for (base_class_info const& base: bases_)
		{
			managed_shared_ptr_ptr base_ptr = base.managed_shared_ptr_ptr_upcast(ptr);
			assert(base_ptr != nullptr);
			managed_shared_ptr_ptr result =
				base.info->managed_shared_ptr_ptr_upcast(base_ptr, type);
			if (result != nullptr) { return result; }
		}
		return nullptr;
	}
	
	template <typename T>
	void add_object(v8::Isolate* isolate,
									T* object,
									persistent<v8::Object>&& handle,
									bool can_modify,
									bool claim_ownership,
									bool count_against_vm_size,
									const std::function<size_t(const T*)>& obj_size_func) {
		// exception-throwing checks moved to class_singleton so they could
		// check to make sure no base class 
		assert(object_records_.find(object) == object_records_.end());
		object_record orec(std::move(handle),
											 can_modify,
											 claim_ownership,
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
	 bool can_modify,
	 bool count_against_vm_size,
	 const std::function<size_t(const T*)>& obj_size_func)
	{
		assert(object_records_.find(object.get()) == object_records_.end());
		managed_shared_ptr_ptr mspp(new std::shared_ptr<T>(object));
		object_record orec(std::move(handle), std::move(mspp), can_modify,
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

	bool can_modify_object(void* obj) {
		auto it = object_records_.find(obj);
		if (it == object_records_.end()) { return false; }
		return it->second.can_modify;
	}
	
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
			if (!has_shared_ptr && (destroy_func != nullptr) && it->second.destroy) {
				destroy_func(object);
			}
			object_records_.erase(object);
		}
	}

	template<typename T>
	void remove_objects(v8::Isolate* isolate,
											const std::function<void(T*)>& destroy_func,
											const std::function<size_t(const T*)>& obj_size_func)
	{
		for (auto& object_rec: object_records_)
		{
			bool has_shared_ptr = object_rec.second.has_shared_ptr();
			object_rec.second.v8object.Reset();
			T* obj = //const_cast<T*>(static_cast<T*>(object_rec.first));
				static_cast<T*>(const_cast<void*>(object_rec.first));
			if (object_rec.second.count_against_vm_size &&
					(obj_size_func != nullptr))
			{
				size_t sz = obj_size_func(obj);
				isolate->AdjustAmountOfExternalAllocatedMemory
					(-static_cast<int64_t>(sz));
			}
			if (!has_shared_ptr && (destroy_func != nullptr) &&
					object_rec.second.destroy)
			{
				destroy_func(obj);
			}
		}
	}

	const object_record* find_object_record
	(void const* object) const {
		auto it = object_records_.find(const_cast<void*>(object));
		if (it != object_records_.end()) {
			return &it->second;
		}
		return nullptr;
	}

	// NOTE: this only works if the pointer you pass in is already
	// the "maximally derived" version of the pointer! If you start with a
	// pointer to a base class, this function will NOT necessarily find the
	// right record in the derived class records, because it doesn't do any
	// pointer adjustements!
	const object_record* find_object_record_searching_derivatives
	(void const* object) const
	{
		auto it = object_records_.find(const_cast<void*>(object));
		if (it != object_records_.end()) {
			return &it->second;
		}
		for (derived_class_info const& dinfo: derivatives_) {
			const object_record* result = dinfo.info->find_object_record(object);
			if (result != nullptr) { return result; }
		}
		return nullptr;
	}

	v8::Local<v8::Object> find_object
	(v8::Isolate* isolate, void const* object) const
	{
		const object_record* orec = find_object_record_searching_derivatives
			(object);
		if (orec != nullptr) {
			return to_local(isolate, orec->v8object);
		}
		return v8::Local<v8::Object>();
	}
	
	managed_shared_ptr_ptr find_managed_shared_ptr_ptr(void const* object) const
	{
		const object_record* orec =
			find_object_record_searching_derivatives(object);
		if (orec != nullptr) {
			return orec->shptr;
		}
		return nullptr;
	}

	// Simple depth-first search through inheritance graph
	bool pointer_already_wrapped_helper
	(void const* object,
	 std::unordered_set<class_info const*>& already_visited) const {
		if (already_visited.find(this) != already_visited.end()) {
			return false;
		}
		already_visited.insert(this);
		auto it = object_records_.find(const_cast<void*>(object));
		if (it != object_records_.end()) {
			return true;
		}
		for (base_class_info const& base: bases_) {
			void const* castobj = base.upcast(object);
			if (base.info->pointer_already_wrapped_helper
					(castobj, already_visited)) {
				return true;
			}
		}
		for (derived_class_info const& deriv: derivatives_) {
			void const* castobj = deriv.downcast(object);
			if (deriv.info->pointer_already_wrapped_helper
					(castobj, already_visited)) {
				return true;
			}
		}
		return false;
	}

	bool pointer_already_wrapped(void const* object) const {
		std::unordered_set<class_info const*> already_visited;
		return pointer_already_wrapped_helper(object, already_visited);
	}
		
protected:
	struct base_class_info
	{
		class_info* info;

		// This function takes a pointer to our type and transforms it to
		// a pointer of the base's type
		cast_function upcast;

		managed_shared_ptr_ptr_cast_function managed_shared_ptr_ptr_upcast;

		base_class_info(class_info* info, cast_function upcst,
										managed_shared_ptr_ptr_cast_function spupcast)
			: info(info)
			, upcast(upcst)
			, managed_shared_ptr_ptr_upcast(spupcast)
		{
		}
	};

	struct derived_class_info
	{
		class_info* info;

		// This function takes a pointer to our type and casts it to a pointer
		// of the derived type
		cast_function downcast;

		derived_class_info(class_info* info, cast_function downcst)
			:info(info)
			, downcast(downcst)
		{
		}
	};
	
	type_info const type_;
	std::vector<base_class_info> bases_;
	std::vector<derived_class_info> derivatives_;

	std::unordered_map<void const*, object_record> object_records_;
	
}; // end class_info

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
		, throw_exception_when_object_not_found_(true)
		, autowrap_shared_(false)
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

	
	bool object_already_wrapped(T const* object) const {
		return pointer_already_wrapped(object);
	}
	
	v8::Handle<v8::Object> wrap(T* object,
															bool can_modify,
															bool claim_ownership,
															bool count_against_vm_size)
	{

		if (object_already_wrapped(object)) {
			throw std::runtime_error
				(type().name() + " (or super/subclass) already wrapped: " +
				 pointer_str(object));
		}
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
													 can_modify,
													 claim_ownership,
													 count_against_vm_size,
													 object_size_func_);

		return scope.Escape(obj);
	}

	v8::Handle<v8::Object> wrap_shared(std::shared_ptr<T> object,
																		 bool can_modify,
																		 bool count_against_vm_size)
	{

		if (object_already_wrapped(object.get())) {
			throw std::runtime_error
				(type().name() + " (or super/subclass) already wrapped: " +
				 pointer_str(object.get()));
		}
		
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
																	can_modify,
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

	bool get_count_shared_against_vm_size() const
	{
		return count_shared_as_externally_allocated_;
	}

	void set_throw_exception_when_object_not_found(bool t) {
		throw_exception_when_object_not_found_ = t;
	}
	
	bool get_throw_exception_when_object_not_found() const {
		return throw_exception_when_object_not_found_;
	}

	void set_autowrap_shared(bool a) {
		autowrap_shared_ = a;
	}

	bool get_autowrap_shared() const {
		return autowrap_shared_;
	}
	
	template<typename U>
	void inherit()
	{
		class_singleton<U>* base = &class_singletons::find_class<U>(isolate_);
		add_base(base,
			[](void const* ptr) -> void const*
			{
				return static_cast<U const*>(static_cast<T const*>(ptr));
			},
			[](void const* ptr) -> void const*
			{
			  return static_cast<T const*>(static_cast<U const*>(ptr));
  	  },
			[](const managed_shared_ptr_ptr& p) -> managed_shared_ptr_ptr
			{
				std::shared_ptr<T>* tp = static_cast<std::shared_ptr<T>*>(p.get());
				std::shared_ptr<U>* up = new std::shared_ptr<U>(*tp);
				return managed_shared_ptr_ptr(up);
			});
		js_function_template()->Inherit(base->class_function_template());
	}

	template<typename U>
	void virtually_inherit()
	{
		class_singleton<U>* base = &class_singletons::find_class<U>(isolate_);
		add_base(base,
			[](void const* ptr) -> void const*
			{
				return static_cast<U const*>(static_cast<T const*>(ptr));
			},
			[](void const* ptr) -> void const*
			{
			  return dynamic_cast<T const*>(static_cast<U const*>(ptr));
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
		return wrap(object, true, false, false);
	}
	v8::Handle<v8::Object> wrap_external_const_object(T const* object)
	{
		return wrap(const_cast<T*>(object), false, false, false);
	}
	v8::Handle<v8::Object> wrap_object(T* object)
	{
		return wrap(object, true, true, true);
	}
	v8::Handle<v8::Object> wrap_const_object(T const* object)
	{
		return wrap(const_cast<T*>(object), false, true, true);
	}
	
	v8::Handle<v8::Object> wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args)
	{

		if (ctor_) {
			return wrap_object(ctor_(args));
		} else if (shared_ctor_) {
			return wrap_shared(shared_ctor_(args), true,
												 count_shared_as_externally_allocated_);
		} else {
			throw std::runtime_error(class_name(type()) + " has no constructor");
		}
	}

  T const* unwrap_const_object(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);

		while (value->IsObject())
		{
			v8::Handle<v8::Object> obj = value->ToObject();
			if (obj->InternalFieldCount() == 2)
			{
				void* ptr = obj->GetAlignedPointerFromInternalField(0);
				if (ptr == nullptr) {
					throw std::runtime_error
						(class_name(type()) + ": C++ object already removed");
				}
				class_info* info = static_cast<class_info*>(obj->GetAlignedPointerFromInternalField(1));
				assert(info->find_object_record(ptr) != nullptr); // added by SD
				if (info && info->upcast(ptr, type()))
				{
					return static_cast<T const*>(ptr);
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
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
				if (ptr == nullptr) {
					throw std::runtime_error
						(class_name(type()) + ": C++ object already removed");
				}
				
				class_info* info = static_cast<class_info*>(obj->GetAlignedPointerFromInternalField(1));
				const object_record* orec = info->find_object_record(ptr);
				assert(orec != nullptr);
				if (info && info->upcast(ptr, type()))
				{
					if (orec->can_modify) {
						return static_cast<T*>(ptr);
					} else {
						throw std::runtime_error
							("Attempt to unwrap const C++ object (" + type().name() +
							 ") for non-const access");
						return nullptr;
					}
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
					const object_record* orec = info->find_object_record(ptr);
					assert(orec != nullptr);
					if (!orec->shptr)
					{
						throw std::runtime_error
							("Attempt to unwrap shared_ptr<" +
							 info->type().name() + "> for non-shared object");
					}
					if (!orec->can_modify)
					{
						throw std::runtime_error
							("Attempt to unwrap const C++ object (" + type().name() +
							 ") for non-const access");
						return nullptr;
					}
					managed_shared_ptr_ptr cast_mspp = info->managed_shared_ptr_ptr_upcast
							(orec->shptr, type());
					if (cast_mspp != nullptr)
					{
						return *(static_cast<std::shared_ptr<T>*>(cast_mspp.get()));
					}
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
	}

	std::shared_ptr<T const> unwrap_const_shared_object
	(v8::Local<v8::Value> value)
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
					const object_record* orec = info->find_object_record(ptr);
					assert(orec != nullptr);
					if (!orec->shptr)
					{
						throw std::runtime_error
							("Attempt to unwrap shared_ptr<" +
							 info->type().name() + "> for non-shared object");
					}
					managed_shared_ptr_ptr cast_mspp = info->managed_shared_ptr_ptr_upcast
							(orec->shptr, type());
					if (cast_mspp != nullptr)
					{
						return *(static_cast<std::shared_ptr<T>*>(cast_mspp.get()));
					}
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
	}	
	
	v8::Handle<v8::Object> find_object_or_empty(T const* obj) const
	{
		return class_info::find_object(isolate_, obj);
	}
	
	v8::Handle<v8::Object> find_object(T const* obj)
	{
		auto result = class_info::find_object(isolate_, obj);
		if (result.IsEmpty() && throw_exception_when_object_not_found_) {
			throw std::runtime_error
				("Couldn't find JS wrapper for provided " + type().name());
		}
		return result;
	}

	/*
	v8::Handle<v8::Object> find_shared_object
	(std::shared_ptr<T> obj) {
		auto result = find_object_or_empty(obj.get());
		if (result.IsEmpty() && autowrap_shared_) {
			return wrap_shared(obj, !std::is_const<T>::value,
												 count_shared_as_externally_allocated_);
		} else if (result.IsEmpty() && throw_exception_when_object_not_found_) {
			throw std::runtime_error
				("Couldn't find JS wrapper for provided (shared) " + type().name());
		}
		return result;
	}
	*/

	v8::Handle<v8::Object> find_shared_object
	(std::shared_ptr<T> obj) {
		auto result = find_object_or_empty(obj.get());
		if (result.IsEmpty() && autowrap_shared_) {
			return wrap_shared(obj, true,
												 count_shared_as_externally_allocated_);
		} else if (result.IsEmpty() && throw_exception_when_object_not_found_) {
			throw std::runtime_error
				("Couldn't find JS wrapper for provided (shared) " + type().name());
		}
		return result;
	}

	v8::Handle<v8::Object> find_const_shared_object
	(std::shared_ptr<T const> obj) {
		auto result = find_object_or_empty(obj.get());
		if (result.IsEmpty() && autowrap_shared_) {
			return wrap_shared(std::const_pointer_cast<T>(obj), false,
												 count_shared_as_externally_allocated_);
		} else if (result.IsEmpty() && throw_exception_when_object_not_found_) {
			throw std::runtime_error
				("Couldn't find JS wrapper for provided (shared) " + type().name());
		}
		return result;
	}

	
	/*
	v8::Handle<v8::Object> find_or_wrap_shared_object
	(std::shared_ptr<T> obj)
	{
		auto result = class_info::find_object(isolate_, obj.get());
		if (result.IsEmpty()) {
			return wrap_shared(obj, true, count_shared_as_externally_allocated_);
		}
		return result;
	}
	*/

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
	bool throw_exception_when_object_not_found_;
	bool autowrap_shared_;
	
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

	class_& set_throw_exception_when_object_not_found(bool t) {
		class_singleton_.set_throw_exception_when_object_not_found(t);
		return *this;
	}
	
	class_& set_autowrap_shared(bool a) {
		class_singleton_.set_autowrap_shared(a);
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

	template<typename U>
	class_& virtually_inherit()
	{
		static_assert(std::is_base_of<U, T>::value, "Class U should be base for class T");
		//TODO: std::is_convertible<T*, U*> and check for duplicates in hierarchy?
		class_singleton_.template virtually_inherit<U>();
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
		return detail::class_singletons::find_class<T>(isolate).
			wrap_external_object(ext);
	}

	static v8::Handle<v8::Object> reference_const_external
	(v8::Isolate* isolate, T const* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).
			wrap_external_const_object(ext);		
	}
	
	static void remove_object(v8::Isolate* isolate, T* obj)
	{
		detail::class_singletons::find_class<T>(isolate).remove_object(obj);
	}
	
	/// As reference_external but delete memory for C++ object
	/// when JavaScript object is deleted. You must use "new" to allocate ext.
	static v8::Handle<v8::Object> import_external(v8::Isolate* isolate, T* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).wrap_object(ext);
	}

	static v8::Handle<v8::Object> import_const_external(v8::Isolate* isolate,
																											T const* ext)
	{
		return detail::class_singletons::find_class<T>(isolate).wrap_const_object
			(ext);		
	}
	
	/// Get wrapped object from V8 value, may return nullptr on fail.
	static T* unwrap_object(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return detail::class_singletons::find_class<T>(isolate).
			unwrap_object(value);
	}

	static T const* unwrap_const_object(v8::Isolate* isolate,
																			v8::Handle<v8::Value> value) {
		return detail::class_singletons::find_class<T>(isolate).
			unwrap_const_object(value);
	}
	
	static std::shared_ptr<T> unwrap_shared_object
	(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return detail::class_singletons::find_class<T>(isolate).
			unwrap_shared_object(value);
	}

	static std::shared_ptr<T const> unwrap_const_shared_object
	(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return detail::class_singletons::find_class<T>(isolate).
			unwrap_const_shared_object(value);
	}

	
	static v8::Handle<v8::Object> wrap_shared_object(v8::Isolate* isolate,
																									 std::shared_ptr<T> obj)
	{
		auto& c = detail::class_singletons::find_class<T>(isolate);
		return c.wrap_shared
			(obj, true, c.get_count_shared_against_vm_size());
	}

	static v8::Handle<v8::Object> wrap_const_shared_object
	(v8::Isolate* isolate, std::shared_ptr<T const> obj)
	{
		auto& c = detail::class_singletons::find_class<T>(isolate);
		return c.wrap_shared
			(std::const_pointer_cast<T>(obj), false,
			 c.get_count_shared_against_vm_size());
	}
	
	static bool object_already_wrapped(v8::Isolate* isolate, T const* obj) {
		auto& c = detail::class_singletons::find_class<T>(isolate);
		return c.object_already_wrapped(obj);
	}

	static bool object_already_wrapped(v8::Isolate* isolate,
																		 std::shared_ptr<T const> obj) {
		return object_already_wrapped(isolate, obj.get());
	}

	static bool object_already_wrapped_as_this_class
	(v8::Isolate* isolate, T const* obj) {
		auto& c = detail::class_singletons::find_class<T>(isolate);
		return (c.find_object_record(obj) != nullptr);
	}

	static bool object_already_wrapped_as_this_class
	(v8::Isolate* isolate, std::shared_ptr<T const> obj) {
		return object_already_wrapped_as_this_class(isolate, obj.get());
	}
	
	static bool object_already_wrapped_as_different_class
	(v8::Isolate* isolate, T const* obj) {
		auto& c = detail::class_singletons::find_class<T>(isolate);
		if (c.find_object_record(obj) != nullptr) {
			return false;
		}
		return c.object_already_wrapped(obj);
	}

	static bool object_already_wrapped_as_different_class
	(v8::Isolate* isolate, std::shared_ptr<T const> obj) {
		return object_already_wrapped_as_different_class(isolate, obj.get());
	}
	
	/// Find V8 object handle for a wrapped C++ object.
	static v8::Handle<v8::Object> find_object(v8::Isolate* isolate, T const* obj)
	{
		return detail::class_singletons::find_class<T>(isolate).find_object(obj);
	}

	static v8::Handle<v8::Object> find_object_or_empty
	(v8::Isolate* isolate, T const* obj) {
		return detail::class_singletons::find_class<T>(isolate).
			find_object_or_empty(obj);
	}
	
	/*
	static v8::Handle<v8::Object> find_object(v8::Isolate* isolate,
																						std::shared_ptr<T> optr) {
		return find_object(optr.get());
	}
	*/
	/*
	static v8::Handle<v8::Object> find_or_wrap_shared_object
	(v8::Isolate* isolate, std::shared_ptr<T> optr) {
		return detail::class_singletons::find_class<T>(isolate).
			find_or_wrap_shared_object(optr);
	}
	*/

	static v8::Handle<v8::Object> find_shared_object
	(v8::Isolate* isolate, std::shared_ptr<T> optr) {
		return detail::class_singletons::find_class<T>(isolate).
			find_shared_object(optr);
	}

	static v8::Handle<v8::Object> find_const_shared_object
	(v8::Isolate* isolate, std::shared_ptr<T const> optr) {
		return detail::class_singletons::find_class<T>(isolate).
			find_const_shared_object(optr);
	}

#if 0	
	static v8::Handle<v8::Object> find_shared_object_or_null
	(v8::Isolate* isolate, std::shared_ptr<T> optr) {
		return detail::class_singletons::find_class<T>(isolate).
			find_object_or_null(optr.get());
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
