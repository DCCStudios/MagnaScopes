#pragma once
#include "RE/N/NiMatrix3.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiPoint4.h"
#include <cmath>
#include <cstdint>
#include <d3d11.h>

#ifndef MATH_PI
#	define MATH_PI 3.14159265358979323846
#endif
using RE::NiMatrix3;
using RE::NiPoint3;

float toRad = (float)(MATH_PI / 180.0);



typedef std::array<float, 3> float3;
typedef std::array<float3, 3> float3x3;

const float PI = 3.14159265358979323846264f;

namespace detail
{

	// 通用向量复制工具
	template <typename TDst, typename TSrc>
	inline void CopyVector3(TDst& dst, const TSrc& src)
	{
		dst.x = src.x;
		dst.y = src.y;
		dst.z = src.z;
	}

	template <typename TDst, typename TSrc>
	inline void CopyVector2(TDst& dst, const TSrc& src)
	{
		dst.x = src.x;
		dst.y = src.y;
	}
}

bool closeEnough(const float& a, const float& b, const float& epsilon = std::numeric_limits<float>::epsilon())
{
	return (epsilon > std::abs(a - b));
}

float3 eulerAngles(const float3x3& R)
{
	//check for gimbal lock
	if (closeEnough(R[0][2], -1.0f)) {
		float x = 0;  //gimbal lock, value of x doesn't matter
		float y = PI / 2;
		float z = x + atan2(R[1][0], R[2][0]);
		return { x, y, z };
	} else if (closeEnough(R[0][2], 1.0f)) {
		float x = 0;
		float y = -PI / 2;
		float z = -x + atan2(-R[1][0], -R[2][0]);
		return { x, y, z };
	} else {  //two solutions exist
		float x1 = -asin(R[0][2]);
		float x2 = PI - x1;

		float y1 = atan2(R[1][2] / cos(x1), R[2][2] / cos(x1));
		float y2 = atan2(R[1][2] / cos(x2), R[2][2] / cos(x2));

		float z1 = atan2(R[0][1] / cos(x1), R[0][0] / cos(x1));
		float z2 = atan2(R[0][1] / cos(x2), R[0][0] / cos(x2));

		//choose one solution to return
		//for example the "shortest" rotation
		if ((std::abs(x1) + std::abs(y1) + std::abs(z1)) <= (std::abs(x2) + std::abs(y2) + std::abs(z2))) {
			return { x1, y1, z1 };
		} else {
			return { x2, y2, z2 };
		}
	}
}


struct Quaternion
{
public:
	float x, y, z, w;
	Quaternion(float _x, float _y, float _z, float _w);
	float Norm();
	NiMatrix3 ToRotationMatrix33();
};

Quaternion::Quaternion(float _x, float _y, float _z, float _w)
{
	x = _x;
	y = _y;
	z = _z;
	w = _w;
}

float Quaternion::Norm()
{
	return x * x + y * y + z * z + w * w;
}


class NiMatrix4
{
public:
	void MakeIdentity() noexcept
	{
		entry[0] = { 1.0F, 0.0F, 0.0F, 0.0F };
		entry[1] = { 0.0F, 1.0F, 0.0F, 0.0F };
		entry[2] = { 0.0F, 0.0F, 1.0F, 0.0F };
		entry[3] = { 0.0F, 0.0F, 0.0F, 1.0F };
	}

	// members
	RE::NiPoint4 entry[4];  // 00


	RE::NiPoint4 operator*(const RE::NiPoint4& p) const
	{
		return RE::NiPoint4{
			entry[0].x * p.x + entry[0].y * p.y + entry[0].z * p.z + entry[0].w * p.w,
			entry[1].x * p.x + entry[1].y * p.y + entry[1].z * p.z + entry[1].w * p.w,
			entry[2].x * p.x + entry[2].y * p.y + entry[2].z * p.z + entry[2].w * p.w,
			entry[3].x * p.x + entry[3].y * p.y + entry[3].z * p.z + entry[3].w * p.w
		};
	}
};

NiMatrix4 ToQ(const RE::NiMatrix3& p)
{
	NiMatrix4 tmp;
	tmp.entry[0] = p.entry[0];
	tmp.entry[1] = p.entry[1];
	tmp.entry[2] = p.entry[2];
	tmp.entry[3] = { 0, 0, 0, 1 };

	return tmp;
}

RE::NiPoint3 MatMulNi3(RE::NiMatrix3 mat, RE::NiPoint3 p)
{
	return RE::NiPoint3(
		mat.entry[0].x * p.x + mat.entry[0].y * p.y + mat.entry[0].z * p.z,
		mat.entry[1].x * p.x + mat.entry[1].y * p.y + mat.entry[1].z * p.z,
		mat.entry[2].x * p.x + mat.entry[2].y * p.y + mat.entry[2].z * p.z);
}




void SetMatrix33(float a, float b, float c, float d, float e, float f, float g, float h, float i, NiMatrix3& mat)
{
	mat.entry[0].x = a;
	mat.entry[0].y = b;
	mat.entry[0].z = c;
	mat.entry[1].x = d;
	mat.entry[1].y = e;
	mat.entry[1].z = f;
	mat.entry[2].x = g;
	mat.entry[2].y = h;
	mat.entry[2].z = i;
}

NiMatrix3 Transpose(NiMatrix3 mat)
{
	NiMatrix3 trans;
	float a = mat.entry[0].x;
	float b = mat.entry[0].y;
	float c = mat.entry[0].z;
	float d = mat.entry[1].x;
	float e = mat.entry[1].y;
	float f = mat.entry[1].z;
	float g = mat.entry[2].x;
	float h = mat.entry[2].y;
	float i = mat.entry[2].z;
	SetMatrix33(a, d, g,
		b, e, h,
		c, f, i, trans);
	return trans;
}

NiMatrix3 GetRotationMatrix33(float pitch, float yaw, float roll)
{
	NiMatrix3 m_roll;
	SetMatrix33(cos(roll), -sin(roll), 0,
		sin(roll), cos(roll), 0,
		0, 0, 1,
		m_roll);
	NiMatrix3 m_yaw;
	SetMatrix33(1, 0, 0,
		0, cos(yaw), -sin(yaw),
		0, sin(yaw), cos(yaw),
		m_yaw);
	NiMatrix3 m_pitch;
	SetMatrix33(cos(pitch), 0, sin(pitch),
		0, 1, 0,
		-sin(pitch), 0, cos(pitch),
		m_pitch);
	return m_roll * m_pitch * m_yaw;
}

NiMatrix3 GetRotationMatrix33(NiPoint3 axis, float angle)
{
	float x = axis.x * sin(angle / 2.0f);
	float y = axis.y * sin(angle / 2.0f);
	float z = axis.z * sin(angle / 2.0f);
	float w = cos(angle / 2.0f);
	Quaternion q = Quaternion(x, y, z, w);
	return q.ToRotationMatrix33();
}

NiMatrix3 GetScaleMatrix(float x, float y, float z)
{
	NiMatrix3 ret;
	SetMatrix33(x, 0, 0, 0, y, 0, 0, 0, z, ret);
	return ret;
}

//From https://android.googlesource.com/platform/external/jmonkeyengine/+/59b2e6871c65f58fdad78cd7229c292f6a177578/engine/src/core/com/jme3/math/Quaternion.java
NiMatrix3 Quaternion::ToRotationMatrix33()
{
	float norm = Norm();
	// we explicitly test norm against one here, saving a division
	// at the cost of a test and branch.  Is it worth it?
	float s = (norm == 1.0f) ? 2.0f : (norm > 0.0f) ? 2.0f / norm :
	                                                  0;

	// compute xs/ys/zs first to save 6 multiplications, since xs/ys/zs
	// will be used 2-4 times each.
	float xs = x * s;
	float ys = y * s;
	float zs = z * s;
	float xx = x * xs;
	float xy = x * ys;
	float xz = x * zs;
	float xw = w * xs;
	float yy = y * ys;
	float yz = y * zs;
	float yw = w * ys;
	float zz = z * zs;
	float zw = w * zs;

	// using s=2/norm (instead of 1/norm) saves 9 multiplications by 2 here
	NiMatrix3 mat;
	SetMatrix33(1 - (yy + zz), (xy - zw), (xz + yw),
		(xy + zw), 1 - (xx + zz), (yz - xw),
		(xz - yw), (yz + xw), 1 - (xx + yy),
		mat);

	return mat;
}

//Sarrus rule
float Determinant(NiMatrix3 mat)
{
	return mat.entry[0].x * mat.entry[1].y * mat.entry[2].z + mat.entry[0].y * mat.entry[1].z * mat.entry[2].x + mat.entry[0].z * mat.entry[1].x * mat.entry[2].y - mat.entry[0].z * mat.entry[1].y * mat.entry[2].x - mat.entry[0].y * mat.entry[1].x * mat.entry[2].z - mat.entry[0].x * mat.entry[1].z * mat.entry[2].y;
}

NiMatrix3 Inverse(NiMatrix3 mat)
{
	float det = Determinant(mat);
	if (det == 0) {
		NiMatrix3 idmat;
		idmat.MakeIdentity();
		return idmat;
	}
	float a = mat.entry[0].x;
	float b = mat.entry[0].y;
	float c = mat.entry[0].z;
	float d = mat.entry[1].x;
	float e = mat.entry[1].y;
	float f = mat.entry[1].z;
	float g = mat.entry[2].x;
	float h = mat.entry[2].y;
	float i = mat.entry[2].z;
	NiMatrix3 invmat;
	SetMatrix33(e * i - f * h, -(b * i - c * h), b * f - c * e,
		-(d * i - f * g), a * i - c * g, -(a * f - c * d),
		d * h - e * g, -(a * h - b * g), a * e - b * d,
		invmat);
	return invmat * (1.0f / det);
}

NiPoint3 ToDirectionVector(NiMatrix3 mat)
{
	return NiPoint3(mat.entry[2].x, mat.entry[2].y, mat.entry[2].z);
}

NiPoint3 ToUpVector(NiMatrix3 mat)
{
	return NiPoint3(mat.entry[1].x, mat.entry[1].y, mat.entry[1].z);
}

//(Rotation Matrix)^-1 * (World pos - Local Origin)
NiPoint3 WorldToLocal(NiPoint3 wpos, NiPoint3 lorigin, NiMatrix3 rot)
{
	NiPoint3 lpos = wpos - lorigin;
	NiMatrix3 invrot = Inverse(rot);


	return NiPoint3(
		invrot.entry[0].x * lpos.x + invrot.entry[0].y * lpos.y + invrot.entry[0].z * lpos.z,
		invrot.entry[1].x * lpos.x + invrot.entry[1].y * lpos.y + invrot.entry[1].z * lpos.z,
		invrot.entry[2].x * lpos.x + invrot.entry[2].y * lpos.y + invrot.entry[2].z * lpos.z);
}

NiPoint3 LocalToWorld(NiPoint3 lpos, NiPoint3 lorigin, NiMatrix3 rot)
{
	return NiPoint3(
			   rot.entry[0].x * lpos.x + rot.entry[0].y * lpos.y + rot.entry[0].z * lpos.z,
			   rot.entry[1].x * lpos.x + rot.entry[1].y * lpos.y + rot.entry[1].z * lpos.z,
			   rot.entry[2].x * lpos.x + rot.entry[2].y * lpos.y + rot.entry[2].z * lpos.z) 
		
	       + lorigin;
}

NiPoint3 CrossProduct(NiPoint3 a, NiPoint3 b)
{
	NiPoint3 ret;
	ret[0] = a[1] * b[2] - a[2] * b[1];
	ret[1] = a[2] * b[0] - a[0] * b[2];
	ret[2] = a[0] * b[1] - a[1] * b[0];
	if (ret[0] == 0 && ret[1] == 0 && ret[2] == 0) {
		if (a[2] != 0) {
			ret[0] = 1;
		} else {
			ret[1] = 1;
		}
	}
	return ret;
}

float DotProduct(NiPoint3 a, NiPoint3 b);
float Length(NiPoint3 p);
NiPoint3 Normalize(NiPoint3 p);

