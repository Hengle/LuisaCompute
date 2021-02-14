//
// Created by Mike Smith on 2021/2/2.
//

#include <string>
#include <fstream>
#include <memory>
#include <variant>
#include <atomic>
#include <iostream>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <core/logging.h>
#include <core/arena.h>
#include <core/hash.h>
#include <ast/type.h>
#include <core/union.h>

#include <ast/expression.h>
#include <ast/statement.h>
#include <ast/variable.h>

struct Test {

    std::string s;
    int a;

    template<typename Archive>
    void serialize(Archive &&ar) noexcept {
        ar(s, a);
    }
};

using namespace luisa;
using namespace luisa::compute;

struct alignas(32) AA {
    float4 x;
    float ba[16];
    float a;
    std::atomic<int> atomic;
};

struct BB {
    AA a;
    float b;
    uchar c;
    float3x3 m;
};

LUISA_STRUCT(AA, x, ba, a, atomic)
LUISA_STRUCT(BB, a, b, c, m)

struct Interface : public Noncopyable {
    Interface() noexcept = default;
    Interface(Interface &&) noexcept = default;
    Interface &operator=(Interface &&) noexcept = default;
    ~Interface() noexcept = default;
};

struct Impl : public Interface {};

template<int max_level = -1>
void print(const Type *info, int level = 0) {

    std::string indent_string;
    for (auto i = 0; i < level; i++) { indent_string.append("  "); }
    if (max_level >= 0 && level > max_level) {
        std::cout << indent_string << info->description() << "\n";
        return;
    }

    std::cout << indent_string << Type::tag_name(info->tag()) << ": {\n"
              << indent_string << "  index:       " << info->index() << "\n"
              << indent_string << "  size:        " << info->size() << "\n"
              << indent_string << "  alignment:   " << info->alignment() << "\n"
              << indent_string << "  hash:        " << info->hash() << "\n"
              << indent_string << "  description: " << info->description() << "\n";

    if (info->is_structure()) {
        std::cout << indent_string << "  members:\n";
        for (auto m : info->members()) { print<max_level>(m, level + 2); }
    } else if (info->is_vector() || info->is_array() || info->is_matrix()) {
        std::cout << indent_string << "  dimension:   " << info->element_count() << "\n";
        std::cout << indent_string << "  element:\n";
        print<max_level>(info->element(), level + 2);
    } else if (info->is_atomic()) {
        std::cout << indent_string << "  element:\n";
        print<max_level>(info->element(), level + 2);
    }
    std::cout << indent_string << "}\n";
}

int main() {

    using namespace luisa;
    log_level_verbose();

    LUISA_VERBOSE("verbose...");
    LUISA_VERBOSE_WITH_LOCATION("verbose with {}...", "location");
    LUISA_INFO("info...");
    LUISA_INFO_WITH_LOCATION("info with location...");
    LUISA_WARNING("warning...");
    LUISA_WARNING_WITH_LOCATION("warning with location...");

    LUISA_INFO("size = {}, alignment = {}", sizeof(AA), alignof(AA));
    LUISA_INFO("size = {}, alignment = {}", sizeof(BB), alignof(BB));

    Union<int, float, void *, Interface> un{5};
    Union<int, float, void *, Interface> un2{Interface{}};
    LUISA_INFO("holds-float: {}, as-int: {}", un.holds<float>(), un.as<int>());
    un.emplace(2.0f);
    un.dispatch([](auto &x) noexcept {
        using T = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int>) {
            LUISA_INFO("int: {}", x);
            x = 3;
        } else if constexpr (std::is_same_v<T, float>) {
            LUISA_INFO("float: {}", x);
            x = 1.5f;
        } else {
            LUISA_INFO("unknown...");
        }
    });

    un.emplace<Interface>();
    auto another_un = std::move(un);
    another_un.assign(std::move(un2));

    Arena arena;
    ArenaVector<int> vec{arena, {1, 2, 3}};
    vec.emplace_back(4);

    ArenaString s{arena, "hello, world"};
    LUISA_INFO("{}", s);

    int a[5]{1, 2, 3, 4, 5};
    auto hh = Hash{}(a);

    Arena another{std::move(arena)};
    auto p = another.allocate<int, 1024>(1);
    LUISA_INFO("{}", fmt::ptr(p.data()));

    auto &&type_aa = typeid(AA);
    auto &&type_bb = typeid(BB);
    LUISA_INFO("{}", type_aa.before(type_bb));

    LUISA_INFO("trivially destructible: {}", std::is_trivially_destructible_v<Impl>);

    print(Type::from("array<array<vector<float,3>,5>,9>"));

    auto hash_aa = luisa::xxh32_hash32(type_aa.name(), std::strlen(type_aa.name()), 0);
    auto hash_bb = luisa::xxh3_hash64(type_bb.name(), std::strlen(type_bb.name()), 0);
    LUISA_INFO("{} {}", hash_aa, hash_bb);

    LUISA_INFO("{}", Type::of<std::array<float, 5>>()->description());

    int aa[1024];
    print(Type::of(aa));

    BB bb;
    print(Type::of(bb));

    static_assert(alignof(float3) == 16);

    auto u = float2(1.0f, 2.0f);
    auto v = float3(1.0f, 2.0f, 3.0f);
    auto w = float3(u, 1.0f);

    auto vv = v + w;
    auto bvv = v == w;
    static_assert(std::is_same_v<decltype(bvv), bool3>);
    v += w;
    v *= 10.0f;

    v = 2.0f * v;
    auto ff = v[2];
    ff = 1.0f;
    auto tt = float2{v};

    LUISA_INFO("All registered types:");
    Type::for_each([](auto t) noexcept { print<0>(t); });
}