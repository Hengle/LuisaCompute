#pragma once
#include <type_traits>
#include <stdint.h>
#include <memory>
#include "Pool.h"
#include "vector.h"
#include "Hash.h"
#include "MetaLib.h"

template<typename K, typename V, typename Hash = vengine::hash<K>, typename Equal = std::equal_to<K>, bool useVEngineAlloc = true>
class HashMap {
public:
	using KeyType = K;
	using ValueType = V;
	using HashType = Hash;
	using EqualType = Equal;

private:
	struct LinkNode {
		K key;
		V value;
		LinkNode* last = nullptr;
		LinkNode* next = nullptr;
		uint64_t arrayIndex;
		LinkNode() noexcept {}
		template<typename A, typename B>
		LinkNode(uint64_t arrayIndex, A&& key, B&& value) noexcept : key(std::forward<A>(key)), value(std::forward<B>(value)), arrayIndex(arrayIndex) {}
		template<typename A>
		LinkNode(uint64_t arrayIndex, A&& key) noexcept : key(std::forward<A>(key)), arrayIndex(arrayIndex) {}

		static void Add(LinkNode*& source, LinkNode* dest) noexcept {
			if (!source) {
				source = dest;
			} else {
				if (source->next) {
					source->next->last = dest;
				}
				dest->next = source->next;
				dest->last = source;
				source->next = dest;
			}
		}
	};

public:
	struct Iterator {
		friend class HashMap<K, V, Hash, Equal, useVEngineAlloc>;

	private:
		const HashMap* map;
		uint64_t hashValue;
		HashMap::LinkNode* node;
		inline constexpr Iterator(const HashMap* map, uint64_t hashValue, HashMap::LinkNode* node) noexcept : map(map), hashValue(hashValue), node(node) {}

	public:
		constexpr Iterator() : map(nullptr), hashValue(0), node(nullptr) {}
		inline constexpr bool operator==(const Iterator& a) const noexcept {
			return node == a.node;
		}
		inline constexpr operator bool() const noexcept {
			return node;
		}
		inline constexpr bool operator!=(const Iterator& a) const noexcept {
			return !operator==(a);
		}
		inline K const& Key() const noexcept;
		inline V& Value() const noexcept;
	};
	
private:
	ArrayList<LinkNode*, useVEngineAlloc> allocatedNodes;
	struct HashArray {
	private:
		LinkNode** nodesPtr = nullptr;
		uint64_t mSize;

	public:
		HashArray(HashArray&& map)
			: nodesPtr(map.nodesPtr),
			  mSize(map.mSize) {
			map.mSize = 0;
			map.nodesPtr = nullptr;
			
		}
		uint64_t size() const noexcept { return mSize; }
		constexpr HashArray() noexcept : mSize(0) {}
		void ClearAll() {
			memset(nodesPtr, 0, sizeof(LinkNode*) * mSize);
		}
		template<bool value>
		static void* Allocate(size_t s) noexcept;
		template<>
		static void* Allocate<true>(size_t s) noexcept {
			return vengine_malloc(s);
		}
		template<>
		static void* Allocate<false>(size_t s) noexcept {
			return malloc(s);
		}

		template<bool value>
		static void Free(void* ptr) noexcept;

		template<>
		static void Free<true>(void* ptr) noexcept {
			vengine_free(ptr);
		}
		template<>
		static void Free<false>(void* ptr) noexcept {
			free(ptr);
		}

		HashArray(uint64_t mSize) noexcept : mSize(mSize) {
			nodesPtr = (LinkNode**)Allocate<useVEngineAlloc>(sizeof(LinkNode*) * mSize);
			memset(nodesPtr, 0, sizeof(LinkNode*) * mSize);
		}
		HashArray(HashArray& arr) noexcept : nodesPtr(arr.nodesPtr) {

			mSize = arr.mSize;
			arr.nodesPtr = nullptr;
		}
		void operator=(HashArray& arr) noexcept {
			nodesPtr = arr.nodesPtr;
			mSize = arr.mSize;
			arr.nodesPtr = nullptr;
		}
		void operator=(HashArray&& arr) noexcept {
			operator=(arr);
		}
		~HashArray() noexcept {
			if (nodesPtr) Free<useVEngineAlloc>(nodesPtr);
		}
		LinkNode* const& operator[](uint64_t i) const noexcept {
			return nodesPtr[i];
		}
		LinkNode*& operator[](uint64_t i) noexcept {
			return nodesPtr[i];
		}
	};

	HashArray nodeVec;
	Pool<LinkNode, useVEngineAlloc, true> pool;
	inline static const Hash hsFunc;
	inline static const Equal eqFunc;
	template<typename A, typename B>
	LinkNode* GetNewLinkNode(A&& key, B&& value) {
		LinkNode* newNode = pool.New(allocatedNodes.size(), std::forward<A>(key), std::forward<B>(value));
		allocatedNodes.push_back(newNode);
		return newNode;
	}
	template<typename A>
	LinkNode* GetNewLinkNode(A&& key) {
		LinkNode* newNode = pool.New(allocatedNodes.size(), std::forward<A>(key));
		allocatedNodes.push_back(newNode);
		return newNode;
	}
	void DeleteLinkNode(LinkNode* oldNode) {
		auto ite = allocatedNodes.end() - 1;
		if (*ite != oldNode) {
			(*ite)->arrayIndex = oldNode->arrayIndex;
			allocatedNodes[oldNode->arrayIndex] = *ite;
		}
		allocatedNodes.erase(ite);
		pool.Delete(oldNode);
	}
	inline static constexpr uint64_t GetPow2Size(uint64_t capacity) noexcept {
		uint64_t ssize = 1;
		while (ssize < capacity)
			ssize <<= 1;
		return ssize;
	}
	inline static constexpr uint64_t GetHash(uint64_t hash, uint64_t size) noexcept {
		return hash & (size - 1);
	}
	void Resize(uint64_t newCapacity) noexcept {
		uint64_t capacity = nodeVec.size();
		if (capacity >= newCapacity) return;
		allocatedNodes.reserve(newCapacity);
		HashArray newNode(newCapacity);
		for (uint64_t i = 0; i < allocatedNodes.size(); ++i) {
			LinkNode* node = allocatedNodes[i];
			auto next = node->next;
			node->last = nullptr;
			node->next = nullptr;
			uint64_t hashValue = hsFunc(node->key);
			hashValue = GetHash(hashValue, newCapacity);
			LinkNode*& targetHeaderLink = newNode[hashValue];
			if (!targetHeaderLink) {
				targetHeaderLink = node;
			} else {
				node->next = targetHeaderLink;
				targetHeaderLink->last = node;
				targetHeaderLink = node;
			}
		}
		nodeVec = newNode;
	}
	inline static constexpr Iterator End() noexcept {
		return Iterator(nullptr, -1, nullptr);
	}

public:
	//////////////////Construct & Destruct
	HashMap(uint64_t capacity) noexcept : pool(capacity) {
		if (capacity < 2) capacity = 2;
		capacity = GetPow2Size(capacity);
		nodeVec = HashArray(capacity);
		allocatedNodes.reserve(capacity);
	}
	HashMap(HashMap&& map)
		: allocatedNodes(std::move(map.allocatedNodes)),
		  nodeVec(std::move(nodeVec)),
		  pool (std::move(pool)){
	}

	~HashMap() noexcept {

		for (auto ite = allocatedNodes.begin(); ite != allocatedNodes.end(); ++ite) {
			pool.Delete(*ite);
		}
	}
	HashMap() noexcept : HashMap(16) {}
	///////////////////////
	Iterator Insert(const K& key, const V& value) noexcept {
		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue;

		auto a = nodeVec.size();
		hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				node->value = value;
				return Iterator(this, hashOriginValue, node);
			}
		}

		uint64_t targetCapacity = (uint64_t)((allocatedNodes.size() + 1) / 0.75);
		if (targetCapacity < 16) targetCapacity = 16;
		if (targetCapacity >= nodeVec.size()) {
			Resize(GetPow2Size(targetCapacity));
			hashValue = GetHash(hashOriginValue, nodeVec.size());
		}
		LinkNode* newNode = GetNewLinkNode(key, value);
		LinkNode::Add(nodeVec[hashValue], newNode);
		return Iterator(this, hashOriginValue, newNode);
	}
	Iterator Insert(const K& key, V& value) noexcept {
		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue;

		auto a = nodeVec.size();
		hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				node->value = value;
				return Iterator(this, hashOriginValue, node);
			}
		}

		uint64_t targetCapacity = (uint64_t)((allocatedNodes.size() + 1) / 0.75);
		if (targetCapacity < 16) targetCapacity = 16;
		if (targetCapacity >= nodeVec.size()) {
			Resize(GetPow2Size(targetCapacity));
			hashValue = GetHash(hashOriginValue, nodeVec.size());
		}
		LinkNode* newNode = GetNewLinkNode(key, value);
		LinkNode::Add(nodeVec[hashValue], newNode);
		return Iterator(this, hashOriginValue, newNode);
	}
	Iterator Insert(const K& key, V&& value) noexcept {
		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue;

		auto a = nodeVec.size();
		hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				node->value = std::move(value);
				return Iterator(this, hashOriginValue, node);
			}
		}

		uint64_t targetCapacity = (uint64_t)((allocatedNodes.size() + 1) / 0.75);
		if (targetCapacity < 16) targetCapacity = 16;
		if (targetCapacity >= nodeVec.size()) {
			Resize(GetPow2Size(targetCapacity));
			hashValue = GetHash(hashOriginValue, nodeVec.size());
		}
		LinkNode* newNode = GetNewLinkNode(key, std::move(value));
		LinkNode::Add(nodeVec[hashValue], newNode);
		return Iterator(this, hashOriginValue, newNode);
	}
	Iterator Insert(const K& key) noexcept {
		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue;
		hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				return Iterator(this, hashOriginValue, node);
			}
		}

		uint64_t targetCapacity = (uint64_t)((allocatedNodes.size() + 1) / 0.75);
		if (targetCapacity < 16) targetCapacity = 16;
		if (targetCapacity >= nodeVec.size()) {
			Resize(GetPow2Size(targetCapacity));
			hashValue = GetHash(hashOriginValue, nodeVec.size());
		}
		LinkNode* newNode = GetNewLinkNode(key);
		LinkNode::Add(nodeVec[hashValue], newNode);
		return Iterator(this, hashOriginValue, newNode);
	}

	//void(uint64_t, K const&, V&)
	template<typename Func>
	void IterateAll(const Func& func) const noexcept {
		using FuncType = std::remove_cvref_t<std::remove_pointer_t<Func>>;
		static constexpr size_t ArgSize = FunctionDataType<FuncType>::ArgsCount;
		static constexpr bool noEarlyBreak = std::is_same_v<FunctionDataType<FuncType>::RetType, void>;
		if constexpr (ArgSize == 3) {
			for (uint64_t i = 0; i < allocatedNodes.size(); ++i) {
				LinkNode* vv = allocatedNodes[i];
				if constexpr (noEarlyBreak) {
					func(i, (K const&)vv->key, vv->value);
				} else {
					if (!func(i, (K const&)vv->key, vv->value)) {
						return;
					}
				}
			}
		} else if constexpr (ArgSize == 2) {
			for (uint64_t i = 0; i < allocatedNodes.size(); ++i) {
				LinkNode* vv = allocatedNodes[i];
				if constexpr (noEarlyBreak) {
					func((K const&)vv->key, vv->value);
				} else {
					if (!func((K const&)vv->key, vv->value)) {
						return;
					}
				}
			}
		} else if constexpr (ArgSize == 1) {
			for (uint64_t i = 0; i < allocatedNodes.size(); ++i) {
				LinkNode* vv = allocatedNodes[i];
				if constexpr (noEarlyBreak) {
					func(vv->value);
				} else {
					if (!func(vv->value)) {
						return;
					}
				}
			}
		} else {
			static_assert(false, "Invalid Iterate Functions");
		}
	}
	void Reserve(uint64_t capacity) noexcept {
		uint64_t newCapacity = GetPow2Size(capacity);
		Resize(newCapacity);
	}
	Iterator Find(const K& key) const noexcept {
		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				return Iterator(this, hashOriginValue, node);
			}
		}

		return End();
	}
	void Remove(const K& key) noexcept {

		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		LinkNode*& startNode = nodeVec[hashValue];
		for (LinkNode* node = startNode; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				if (startNode == node) {
					startNode = node->next;
				}
				if (node->next)
					node->next->last = node->last;
				if (node->last)
					node->last->next = node->next;
				DeleteLinkNode(node);
				return;
			}
		}
	}

	void Remove(const Iterator& ite) noexcept {

		uint64_t hashValue = GetHash(ite.hashValue, nodeVec.size());
		if (nodeVec[hashValue] == ite.node) {
			nodeVec[hashValue] = ite.node->next;
		}
		if (ite.node->last)
			ite.node->last->next = ite.node->next;
		if (ite.node->next)
			ite.node->next->last = ite.node->last;
		DeleteLinkNode(ite.node);
	}
	V& operator[](const K& key) noexcept {

		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				return node->value;
			}
		}

		return *(V*)nullptr;
	}
	V const& operator[](const K& key) const noexcept {

		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				return node->value;
			}
		}

		return *(V const*)nullptr;
	}
	bool TryGet(const K& key, V& value) const noexcept {

		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				value = node->value;
				return true;
			}
		}

		return false;
	}
	bool Contains(const K& key) const noexcept {

		uint64_t hashOriginValue = hsFunc(key);
		uint64_t hashValue = GetHash(hashOriginValue, nodeVec.size());
		for (LinkNode* node = nodeVec[hashValue]; node != nullptr; node = node->next) {
			if (eqFunc(node->key, key)) {
				return true;
			}
		}

		return false;
	}
	void Clear() noexcept {
		if (allocatedNodes.empty()) return;
		nodeVec.ClearAll();
		for (auto ite = allocatedNodes.begin(); ite != allocatedNodes.end(); ++ite) {
			pool.Delete(*ite);
		}
		allocatedNodes.clear();
	}
	uint64_t Size() const noexcept { return allocatedNodes.size(); }

	uint64_t GetCapacity() const noexcept { return nodeVec.size(); }
};
template<typename K, typename V, typename Hash, typename Equal, bool useVEngineAlloc>
inline K const& HashMap<K, V, Hash, Equal, useVEngineAlloc>::Iterator::Key() const noexcept {
	return node->key;
}
template<typename K, typename V, typename Hash, typename Equal, bool useVEngineAlloc>
inline V& HashMap<K, V, Hash, Equal, useVEngineAlloc>::Iterator::Value() const noexcept {
	return node->value;
}
