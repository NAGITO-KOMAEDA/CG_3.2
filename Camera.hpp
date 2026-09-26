#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>

class Camera
{
public:
    Camera()
        : m_position(0.0f, 350.0f, -300.0f), m_yaw(0.0f), m_pitch(-0.08f)
    {
    }

    void SetAspect(float aspect)
    {
        m_aspect = std::max(aspect, 0.01f);
    }

    void Rotate(float deltaX, float deltaY)
    {
        constexpr float sensitivity = 0.004f;
        m_yaw += deltaX * sensitivity;
        m_pitch = std::clamp(m_pitch + deltaY * sensitivity, -1.50f, 1.50f);
    }

    void Update(float deltaSeconds, const std::array<bool, 256>& keys)
    {
        using namespace DirectX;

        const XMVECTOR forward = ForwardVector();
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));

        XMVECTOR movement = XMVectorZero();
        if (keys['W']) movement = XMVectorAdd(movement, forward);
        if (keys['S']) movement = XMVectorSubtract(movement, forward);
        if (keys['D']) movement = XMVectorAdd(movement, right);
        if (keys['A']) movement = XMVectorSubtract(movement, right);
        if (keys['E']) movement = XMVectorAdd(movement, worldUp);
        if (keys['Q']) movement = XMVectorSubtract(movement, worldUp);

        if (XMVectorGetX(XMVector3LengthSq(movement)) > 0.0001f)
        {
            movement = XMVector3Normalize(movement);
            constexpr std::size_t ShiftVirtualKey = 0x10;
            const float speed = keys[ShiftVirtualKey] ? 700.0f : 220.0f;
            const XMVECTOR position = XMLoadFloat3(&m_position);
            XMStoreFloat3(&m_position, XMVectorMultiplyAdd(movement, XMVectorReplicate(speed * deltaSeconds), position));
        }
    }

    DirectX::XMMATRIX View() const
    {
        using namespace DirectX;
        const XMVECTOR position = XMLoadFloat3(&m_position);
        return XMMatrixLookToLH(position, ForwardVector(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }

    DirectX::XMMATRIX Projection() const
    {
        return DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(60.0f), m_aspect, 1.0f, 4000.0f);
    }

    const DirectX::XMFLOAT3& Position() const { return m_position; }

private:
    DirectX::XMVECTOR ForwardVector() const
    {
        const float cosPitch = std::cos(m_pitch);
        return DirectX::XMVector3Normalize(DirectX::XMVectorSet(
            std::sin(m_yaw) * cosPitch,
            std::sin(m_pitch),
            std::cos(m_yaw) * cosPitch,
            0.0f));
    }

    DirectX::XMFLOAT3 m_position;
    float m_yaw;
    float m_pitch;
    float m_aspect = 16.0f / 9.0f;
};
