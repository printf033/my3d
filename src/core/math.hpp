#pragma once

// my3d::math —— 引擎自有的最小数学库。
//
// 约束（见 docs/architecture.md §5.1）：
//   * 不依赖任何第三方（尤其不能是 filament::math），否则 scene/ 会隐式拉入 Filament。
//   * Mat4 用列主序 16 个标量连续存放，与 std140 / SPIR-V 布局一致，
//     将来接 Vulkan 或传给 shader 时不需要转换。
//
// 这里提供 Quat，但它有一条明确的边界（曾经这里写的是「不提供 Quat：引入四元数
// 只会多一条需要维护的等价路径，等出现动画/插值需求再加」—— 动画来了，于是它来了）：
// 场景图的变换仍然一律以矩阵表达，四元数**只**用于在关键帧之间插值旋转。
// 两个旋转之间的正确插值需要 slerp，而分量 lerp 用在旋转矩阵上会缩放甚至退化，
// 没有廉价等价物。这是它存在的唯一理由，不是第二种变换表示 —— 采样完立刻 toMat4()。

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <limits>

namespace my3d::math
{

inline constexpr double kPi = 3.14159265358979323846;

template <typename T>
constexpr T radians(T degrees) noexcept
{
    return degrees * static_cast<T>(kPi / 180.0);
}

template <typename T>
constexpr T degrees(T radiansValue) noexcept
{
    return radiansValue * static_cast<T>(180.0 / kPi);
}

template <typename T, typename U, typename V>
constexpr T clamp(T v, U lo, V hi) noexcept
{
    return v < static_cast<T>(lo) ? static_cast<T>(lo)
                                  : (v > static_cast<T>(hi) ? static_cast<T>(hi) : v);
}

template <typename T>
constexpr T lerp(T a, T b, float t) noexcept
{
    return static_cast<T>(a + (b - a) * t);
}

template <typename T>
constexpr T sqr(T v) noexcept
{
    return v * v;
}

template <typename T>
struct TVec2
{
    T x{}, y{};

    constexpr TVec2() noexcept = default;
    constexpr TVec2(T x_, T y_) noexcept : x(x_), y(y_) {}
    template <typename U>
    explicit constexpr TVec2(const TVec2<U> &o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)) {}

    template <typename U>
    constexpr TVec2 &operator+=(const TVec2<U> &o) noexcept
    {
        x = static_cast<T>(x + o.x);
        y = static_cast<T>(y + o.y);
        return *this;
    }
    template <typename U>
    constexpr TVec2 &operator-=(const TVec2<U> &o) noexcept
    {
        x = static_cast<T>(x - o.x);
        y = static_cast<T>(y - o.y);
        return *this;
    }
};

template <typename T>
struct TVec3
{
    T x{}, y{}, z{};

    constexpr TVec3() noexcept = default;
    constexpr TVec3(T x_, T y_, T z_) noexcept : x(x_), y(y_), z(z_) {}
    explicit constexpr TVec3(T s) noexcept : x(s), y(s), z(s) {}
    template <typename U>
    explicit constexpr TVec3(const TVec3<U> &o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)) {}

    constexpr T *data() noexcept { return &x; }
    constexpr const T *data() const noexcept { return &x; }

    template <typename U>
    constexpr TVec3 &operator+=(const TVec3<U> &o) noexcept
    {
        x = static_cast<T>(x + o.x);
        y = static_cast<T>(y + o.y);
        z = static_cast<T>(z + o.z);
        return *this;
    }
    template <typename U>
    constexpr TVec3 &operator-=(const TVec3<U> &o) noexcept
    {
        x = static_cast<T>(x - o.x);
        y = static_cast<T>(y - o.y);
        z = static_cast<T>(z - o.z);
        return *this;
    }
    template <typename U>
    constexpr TVec3 &operator*=(U s) noexcept
    {
        x = static_cast<T>(x * s);
        y = static_cast<T>(y * s);
        z = static_cast<T>(z * s);
        return *this;
    }
};

template <typename T>
struct TVec4
{
    T x{}, y{}, z{}, w{};

    constexpr TVec4() noexcept = default;
    constexpr TVec4(T x_, T y_, T z_, T w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}
    constexpr TVec4(const TVec3<T> &v, T w_) noexcept : x(v.x), y(v.y), z(v.z), w(w_) {}
    template <typename U>
    explicit constexpr TVec4(const TVec4<U> &o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)), w(static_cast<T>(o.w)) {}
};

using Vec2 = TVec2<float>;
using Vec3 = TVec3<float>;
using Vec4 = TVec4<float>;
using DVec2 = TVec2<double>;
using DVec3 = TVec3<double>;
using DVec4 = TVec4<double>;

// ---------------------------------------------------------------- Vec 运算

template <typename T>
constexpr TVec2<T> operator+(const TVec2<T> &a, const TVec2<T> &b) noexcept
{
    return {static_cast<T>(a.x + b.x), static_cast<T>(a.y + b.y)};
}
template <typename T>
constexpr TVec2<T> operator-(const TVec2<T> &a, const TVec2<T> &b) noexcept
{
    return {static_cast<T>(a.x - b.x), static_cast<T>(a.y - b.y)};
}
template <typename T>
constexpr TVec2<T> operator*(const TVec2<T> &a, float s) noexcept
{
    return {static_cast<T>(a.x * s), static_cast<T>(a.y * s)};
}

template <typename T>
constexpr TVec3<T> operator+(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {static_cast<T>(a.x + b.x), static_cast<T>(a.y + b.y), static_cast<T>(a.z + b.z)};
}
template <typename T>
constexpr TVec3<T> operator-(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {static_cast<T>(a.x - b.x), static_cast<T>(a.y - b.y), static_cast<T>(a.z - b.z)};
}
template <typename T>
constexpr TVec3<T> operator-(const TVec3<T> &a) noexcept
{
    return {static_cast<T>(-a.x), static_cast<T>(-a.y), static_cast<T>(-a.z)};
}
template <typename T>
constexpr TVec3<T> operator*(const TVec3<T> &a, float s) noexcept
{
    return {static_cast<T>(a.x * s), static_cast<T>(a.y * s), static_cast<T>(a.z * s)};
}
template <typename T>
constexpr TVec3<T> operator*(float s, const TVec3<T> &a) noexcept
{
    return a * s;
}
template <typename T>
constexpr TVec3<T> operator/(const TVec3<T> &a, float s) noexcept
{
    return {static_cast<T>(a.x / s), static_cast<T>(a.y / s), static_cast<T>(a.z / s)};
}

template <typename T>
constexpr TVec4<T> operator+(const TVec4<T> &a, const TVec4<T> &b) noexcept
{
    return {static_cast<T>(a.x + b.x), static_cast<T>(a.y + b.y),
            static_cast<T>(a.z + b.z), static_cast<T>(a.w + b.w)};
}
template <typename T>
constexpr TVec4<T> operator-(const TVec4<T> &a, const TVec4<T> &b) noexcept
{
    return {static_cast<T>(a.x - b.x), static_cast<T>(a.y - b.y),
            static_cast<T>(a.z - b.z), static_cast<T>(a.w - b.w)};
}

// 分量乘（albedo 混合等）
template <typename T>
constexpr TVec4<T> mul(const TVec4<T> &a, const TVec4<T> &b) noexcept
{
    return {static_cast<T>(a.x * b.x), static_cast<T>(a.y * b.y),
            static_cast<T>(a.z * b.z), static_cast<T>(a.w * b.w)};
}

template <typename T>
constexpr T dot(const TVec2<T> &a, const TVec2<T> &b) noexcept
{
    return static_cast<T>(a.x * b.x + a.y * b.y);
}
template <typename T>
constexpr T dot(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return static_cast<T>(a.x * b.x + a.y * b.y + a.z * b.z);
}
template <typename T>
constexpr TVec3<T> cross(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {static_cast<T>(a.y * b.z - a.z * b.y),
            static_cast<T>(a.z * b.x - a.x * b.z),
            static_cast<T>(a.x * b.y - a.y * b.x)};
}
template <typename T>
constexpr T lengthSq(const TVec3<T> &v) noexcept
{
    return dot(v, v);
}
template <typename T>
inline T length(const TVec3<T> &v) noexcept
{
    return std::sqrt(lengthSq(v));
}
template <typename T>
inline TVec3<T> normalize(const TVec3<T> &v) noexcept
{
    const T len = length(v);
    if (len <= static_cast<T>(1e-20))
        return {};
    return v / static_cast<float>(len);
}
template <typename T>
constexpr T distance(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return length(b - a);
}
// 逐分量乘（albedo 混合等）
template <typename T>
constexpr TVec3<T> mul(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {static_cast<T>(a.x * b.x), static_cast<T>(a.y * b.y), static_cast<T>(a.z * b.z)};
}
template <typename T>
constexpr TVec3<T> min(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
template <typename T>
constexpr TVec3<T> max(const TVec3<T> &a, const TVec3<T> &b) noexcept
{
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
template <typename T>
constexpr TVec3<T> abs(const TVec3<T> &a) noexcept
{
    return {a.x < 0 ? -a.x : a.x, a.y < 0 ? -a.y : a.y, a.z < 0 ? -a.z : a.z};
}
template <typename T>
constexpr TVec3<T> lerp(const TVec3<T> &a, const TVec3<T> &b, float t) noexcept
{
    return a + (b - a) * t;
}

// ------------------------------------------------------------------- Mat3

// 列主序 3x3：m[col * 3 + row]
template <typename T>
struct TMat3
{
    T m[9]{};

    constexpr T &at(int row, int col) noexcept { return m[col * 3 + row]; }
    constexpr const T &at(int row, int col) const noexcept { return m[col * 3 + row]; }

    static constexpr TMat3 identity() noexcept
    {
        TMat3 r;
        r.m[0] = r.m[4] = r.m[8] = static_cast<T>(1);
        return r;
    }
};

using Mat3 = TMat3<float>;

// ------------------------------------------------------------------- Mat4

// 列主序 4x4：m[col * 4 + row]，共 16 个标量连续存放（std140 友好）。
template <typename T>
struct TMat4
{
    T m[16]{};

    constexpr T &at(int row, int col) noexcept { return m[col * 4 + row]; }
    constexpr const T &at(int row, int col) const noexcept { return m[col * 4 + row]; }

    constexpr T *data() noexcept { return m; }
    constexpr const T *data() const noexcept { return m; }

    static constexpr TMat4 identity() noexcept
    {
        TMat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = static_cast<T>(1);
        return r;
    }
};

using Mat4 = TMat4<float>;
using DMat4 = TMat4<double>;

// 精度收窄：相机的 position/front 用 double 累积，最终矩阵转 float。
inline Mat4 toFloat(const DMat4 &m) noexcept
{
    Mat4 r;
    for (int i = 0; i < 16; ++i)
        r.m[i] = static_cast<float>(m.m[i]);
    return r;
}

template <typename T>
constexpr TMat4<T> operator*(const TMat4<T> &a, const TMat4<T> &b) noexcept
{
    TMat4<T> r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            T sum = static_cast<T>(0);
            for (int k = 0; k < 4; ++k)
                sum = static_cast<T>(sum + a.at(row, k) * b.at(k, c));
            r.at(row, c) = sum;
        }
    return r;
}

template <typename T>
constexpr TVec4<T> operator*(const TMat4<T> &a, const TVec4<T> &v) noexcept
{
    return {static_cast<T>(a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w),
            static_cast<T>(a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w),
            static_cast<T>(a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w),
            static_cast<T>(a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w)};
}

// 点变换（w = 1，结果含 w，供透视除法使用）
template <typename T>
constexpr TVec4<T> transformPoint(const TMat4<T> &a, const TVec3<T> &p) noexcept
{
    return a * TVec4<T>(p, static_cast<T>(1));
}

// 方向/法线变换（w = 0，忽略平移）
template <typename T>
constexpr TVec3<T> transformVector(const TMat4<T> &a, const TVec3<T> &v) noexcept
{
    return {static_cast<T>(a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z),
            static_cast<T>(a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z),
            static_cast<T>(a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z)};
}

template <typename T>
constexpr TMat4<T> transpose(const TMat4<T> &a) noexcept
{
    TMat4<T> r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.at(row, c) = a.at(c, row);
    return r;
}

// 一般 4x4 求逆（余子式展开）。奇异时返回单位阵。
template <typename T>
inline TMat4<T> inverse(const TMat4<T> &mat) noexcept
{
    const T *m = mat.m;
    T inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    T det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(det) <= std::numeric_limits<T>::epsilon())
        return TMat4<T>::identity();

    det = static_cast<T>(1) / det;
    TMat4<T> r;
    for (int i = 0; i < 16; ++i)
        r.m[i] = inv[i] * det;
    return r;
}

// 法线矩阵 = transpose(inverse(model))，取左上 3x3。
template <typename T>
inline TMat3<T> normalMatrix(const TMat4<T> &model) noexcept
{
    const TMat4<T> invT = transpose(inverse(model));
    TMat3<T> r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r.at(row, c) = invT.at(row, c);
    return r;
}

template <typename T>
inline TVec3<T> transformNormal(const TMat3<T> &n, const TVec3<T> &v) noexcept
{
    return {static_cast<T>(n.at(0, 0) * v.x + n.at(0, 1) * v.y + n.at(0, 2) * v.z),
            static_cast<T>(n.at(1, 0) * v.x + n.at(1, 1) * v.y + n.at(1, 2) * v.z),
            static_cast<T>(n.at(2, 0) * v.x + n.at(2, 1) * v.y + n.at(2, 2) * v.z)};
}

// ------------------------------------------------------------- 变换构造

template <typename T>
constexpr TMat4<T> translation(const TVec3<T> &t) noexcept
{
    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

template <typename T>
constexpr TMat4<T> scaling(const TVec3<T> &s) noexcept
{
    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

// 绕任意轴旋转（右手，角度为弧度）
template <typename T>
inline TMat4<T> rotation(const TVec3<T> &axis, T angleRad) noexcept
{
    const TVec3<T> a = normalize(axis);
    const T c = std::cos(angleRad);
    const T s = std::sin(angleRad);
    const T t = static_cast<T>(1) - c;

    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 0) = t * a.x * a.x + c;
    r.at(0, 1) = t * a.x * a.y - s * a.z;
    r.at(0, 2) = t * a.x * a.z + s * a.y;
    r.at(1, 0) = t * a.x * a.y + s * a.z;
    r.at(1, 1) = t * a.y * a.y + c;
    r.at(1, 2) = t * a.y * a.z - s * a.x;
    r.at(2, 0) = t * a.x * a.z - s * a.y;
    r.at(2, 1) = t * a.y * a.z + s * a.x;
    r.at(2, 2) = t * a.z * a.z + c;
    return r;
}

// 列向量约定下的 TRS 组合：M = T * R * S
template <typename T>
constexpr TMat4<T> compose(const TVec3<T> &t, const TMat3<T> &rot, const TVec3<T> &s) noexcept
{
    TMat4<T> r = TMat4<T>::identity();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r.at(row, c) = static_cast<T>(rot.at(row, c) * s.data()[c]);
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

// 右手系视图矩阵，相机朝向 -Z。
template <typename T>
inline TMat4<T> lookAt(const TVec3<T> &eye, const TVec3<T> &center, const TVec3<T> &up) noexcept
{
    const TVec3<T> f = normalize(center - eye);
    const TVec3<T> s = normalize(cross(f, up));
    const TVec3<T> u = cross(s, f);

    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
}

// 右手系透视投影，NDC z ∈ [-1, 1]（OpenGL 约定）。
// 光栅器只需要 z 单调，选 GL 约定是因为它更常见、逆变换书写更直观。
template <typename T>
inline TMat4<T> perspective(T fovYRad, T aspect, T nearPlane, T farPlane) noexcept
{
    const T f = static_cast<T>(1) / std::tan(fovYRad * static_cast<T>(0.5));
    TMat4<T> r;   // 零初始化，其余项本来就该是 0
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (farPlane + nearPlane) / (nearPlane - farPlane);
    r.at(2, 3) = (static_cast<T>(2) * farPlane * nearPlane) / (nearPlane - farPlane);
    r.at(3, 2) = static_cast<T>(-1);
    return r;
}

// 正交投影，NDC z ∈ [-1, 1]
template <typename T>
inline TMat4<T> ortho(T l, T r_, T b, T t, T n, T f) noexcept
{
    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 0) = static_cast<T>(2) / (r_ - l);
    r.at(1, 1) = static_cast<T>(2) / (t - b);
    r.at(2, 2) = static_cast<T>(-2) / (f - n);
    r.at(0, 3) = -(r_ + l) / (r_ - l);
    r.at(1, 3) = -(t + b) / (t - b);
    r.at(2, 3) = -(f + n) / (f - n);
    return r;
}

// -------------------------------------------------------------------- Quat

// 单位四元数，分量顺序 (x, y, z, w)，与 glTF / assimp 一致。
//
// 默认构造是零四元数而不是单位四元数 —— 与 TVec 的零初始化保持一致，需要
// 单位量时显式写 identity()。理由：零值是「这个字段还没被赋值」的诚实表达，
// 而一个隐式的单位旋转会把「忘了赋值」伪装成一个看起来合法的姿势。
template <typename T>
struct TQuat
{
    T x{}, y{}, z{}, w{};

    constexpr TQuat() noexcept = default;
    constexpr TQuat(T x_, T y_, T z_, T w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr TQuat identity() noexcept
    {
        return TQuat{static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)};
    }
};

using Quat = TQuat<float>;

template <typename T>
constexpr T dot(const TQuat<T> &a, const TQuat<T> &b) noexcept
{
    return static_cast<T>(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

template <typename T>
constexpr T lengthSquared(const TQuat<T> &q) noexcept
{
    return dot(q, q);
}

template <typename T>
inline T length(const TQuat<T> &q) noexcept
{
    return std::sqrt(dot(q, q));
}

// 退化输入（零四元数 / 非有限值）返回 identity 而不是产生 NaN：调用方多半是
// 「从文件里读出来的数」，一个 NaN 姿势会让整个蒙皮结果变成 NaN，且很难回溯。
template <typename T>
inline TQuat<T> normalize(const TQuat<T> &q) noexcept
{
    const T len = std::sqrt(dot(q, q));
    if (!(len > static_cast<T>(0)))
        return TQuat<T>::identity();
    const T inv = static_cast<T>(1) / len;
    return TQuat<T>(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

// 共轭 == 逆（仅对单位四元数成立，这正是我们唯一的使用场景）。
template <typename T>
constexpr TQuat<T> conjugate(const TQuat<T> &q) noexcept
{
    return TQuat<T>(-q.x, -q.y, -q.z, q.w);
}

// Hamilton 积。注意不可交换 —— 顺序即旋转的复合顺序。
template <typename T>
constexpr TQuat<T> operator*(const TQuat<T> &a, const TQuat<T> &b) noexcept
{
    return TQuat<T>(static_cast<T>(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y),
                    static_cast<T>(a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x),
                    static_cast<T>(a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w),
                    static_cast<T>(a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z));
}

// 单位四元数 → 4x4 旋转矩阵（列主序，与 TMat4 的约定一致）。
//
// 假定输入已归一化：矩阵元素是分量的二次型，未归一化的输入会静默产生一个
// 带缩放的矩阵 —— 不报错、只是模型缩放不对，属于最难查的那类故障。
// 这条契约由调用方（动画采样器）履行，它在写出姿势前必然归一。
template <typename T>
constexpr TMat4<T> toMat4(const TQuat<T> &q) noexcept
{
    const T xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const T xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const T wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    TMat4<T> r = TMat4<T>::identity();
    r.at(0, 0) = static_cast<T>(1) - static_cast<T>(2) * (yy + zz);
    r.at(0, 1) = static_cast<T>(2) * (xy - wz);
    r.at(0, 2) = static_cast<T>(2) * (xz + wy);
    r.at(1, 0) = static_cast<T>(2) * (xy + wz);
    r.at(1, 1) = static_cast<T>(1) - static_cast<T>(2) * (xx + zz);
    r.at(1, 2) = static_cast<T>(2) * (yz - wx);
    r.at(2, 0) = static_cast<T>(2) * (xz - wy);
    r.at(2, 1) = static_cast<T>(2) * (yz + wx);
    r.at(2, 2) = static_cast<T>(1) - static_cast<T>(2) * (xx + yy);
    return r;
}

// 与 compose(t, Mat3, s) 同一语义，只是旋转用四元数给 —— 动画采样器的出参。
template <typename T>
constexpr TMat4<T> compose(const TVec3<T> &t, const TQuat<T> &rot, const TVec3<T> &s) noexcept
{
    TMat4<T> r = toMat4(rot);
    // 缩放乘在【列】上：M = T * R * S 里 S 是右因子，第 c 列整列乘 s[c]。
    // 曾经写成一列循环里固定乘 s.x/s.y/s.z（即按行乘），得到的其实是 S * R。
    // 均匀缩放下两者恰好相等，非均匀缩放下不是一个变换 —— 上面 compose(t, Mat3, s)
    // 一直是按列的，两个重载注释都写着 T * R * S，方向必须一致。
    const T sc[3] = {s.x, s.y, s.z};
    for (int c = 0; c < 3; ++c)
    {
        r.at(0, c) = static_cast<T>(r.at(0, c) * sc[c]);
        r.at(1, c) = static_cast<T>(r.at(1, c) * sc[c]);
        r.at(2, c) = static_cast<T>(r.at(2, c) * sc[c]);
    }
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

// 最短弧 slerp。t 不 clamp 到 [0,1]：调用方已经做过循环取模与区间定位，
// 在这里再夹一次只会掩盖它的错误。
//
// dot < 0 意味着两个四元数落在超球面的对跖半球上 —— 它们描述**同一个旋转**，
// 但按分量插值会绕远路（长弧），表现为骨骼在 180° 附近突然甩一下。把 b 取负
// 翻到同一半球即可，旋转语义不变，这是必须做的一步而不是优化。
template <typename T>
inline TQuat<T> slerp(const TQuat<T> &a, TQuat<T> b, T t) noexcept
{
    T cosTheta = dot(a, b);
    if (cosTheta < static_cast<T>(0))
    {
        b = TQuat<T>(-b.x, -b.y, -b.z, -b.w);
        cosTheta = -cosTheta;
    }

    // 夹角趋零时 sin(theta) → 0，权重里的除法会炸成 inf/NaN。此时两段弧已经
    // 分不出来，退化成归一化线性插值 —— 这一支不仅是兜底，数值上也更准。
    if (cosTheta > static_cast<T>(1) - static_cast<T>(1e-6))
    {
        const TQuat<T> r(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                         a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        return normalize(r);
    }

    const T theta = std::acos(clamp(cosTheta, static_cast<T>(-1), static_cast<T>(1)));
    const T sinTheta = std::sin(theta);
    // 两个端点（t 恰为 0 或 1）走权重公式会得到 0/0 之外的舍入误差，直接返回端点。
    if (t <= static_cast<T>(0))
        return a;
    if (t >= static_cast<T>(1))
        return b;
    const T wa = std::sin((static_cast<T>(1) - t) * theta) / sinTheta;
    const T wb = std::sin(t * theta) / sinTheta;
    return TQuat<T>(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                    a.w * wa + b.w * wb);
}

// -------------------------------------------------------------------- AABB

struct AABB
{
    Vec3 min{};
    Vec3 max{};
    bool valid = false;   // 空盒（无几何）时保持 false，避免用 ±inf 参与剔除

    constexpr AABB() noexcept = default;
    constexpr AABB(const Vec3 &lo, const Vec3 &hi) noexcept : min(lo), max(hi), valid(true) {}

    constexpr Vec3 center() const noexcept { return (min + max) * 0.5f; }
    constexpr Vec3 extent() const noexcept { return (max - min) * 0.5f; }

    constexpr void expand(const Vec3 &p) noexcept
    {
        if (!valid)
        {
            min = max = p;
            valid = true;
            return;
        }
        min = my3d::math::min(min, p);
        max = my3d::math::max(max, p);
    }

    void expand(const AABB &o) noexcept
    {
        if (!o.valid)
            return;
        expand(o.min);
        expand(o.max);
    }

    // 变换到另一空间（变换 8 个角，简单且不会漏）
    AABB transformed(const Mat4 &m) const noexcept
    {
        if (!valid)
            return {};
        AABB out;
        for (int i = 0; i < 8; ++i)
        {
            const Vec3 corner{i & 1 ? max.x : min.x,
                              i & 2 ? max.y : min.y,
                              i & 4 ? max.z : min.z};
            const Vec4 p = transformPoint(m, corner);
            out.expand(Vec3{p.x, p.y, p.z});
        }
        return out;
    }
};

// ------------------------------------------------------------------ Plane

struct Plane
{
    Vec3 normal{};
    float d = 0.0f;   // 平面方程：dot(normal, p) + d = 0
};

struct Frustum
{
    Plane planes[6];   // left, right, bottom, top, near, far

    // 世界空间 AABB 是否与视锥体相交（保守：可能返回「相交」的假阳性，不返回假阴性）
    bool intersects(const AABB &box) const noexcept
    {
        if (!box.valid)
            return false;
        for (const Plane &p : planes)
        {
            // 取沿法线最远的 p-vertex：若它仍在平面外侧，则整个盒子在外侧
            const Vec3 pVertex{p.normal.x >= 0 ? box.max.x : box.min.x,
                               p.normal.y >= 0 ? box.max.y : box.min.y,
                               p.normal.z >= 0 ? box.max.z : box.min.z};
            if (dot(p.normal, pVertex) + p.d < 0.0f)
                return false;
        }
        return true;
    }
};

// Gribb–Hartmann 提取。矩阵为列主序，row_i = (m[i], m[4+i], m[8+i], m[12+i])。
inline Frustum extractFrustum(const Mat4 &viewProj) noexcept
{
    auto row = [&](int i) -> Vec4
    {
        return {viewProj.m[i], viewProj.m[4 + i], viewProj.m[8 + i], viewProj.m[12 + i]};
    };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    const Vec4 combos[6] = {
        r3 + r0,                                  // left
        r3 - r0,                                  // right
        r3 + r1,                                  // bottom
        r3 - r1,                                  // top
        r3 + r2,                                  // near
        r3 - r2,                                  // far
    };

    Frustum f;
    for (int i = 0; i < 6; ++i)
    {
        const Vec4 &p = combos[i];
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len <= 1e-20f)
        {
            f.planes[i] = {{0.0f, 0.0f, 0.0f}, 1.0f};   // 退化平面：不剔除
            continue;
        }
        f.planes[i] = {{p.x / len, p.y / len, p.z / len}, p.w / len};
    }
    return f;
}

} // namespace my3d::math
