#include "rain_overlay.h"

#include <base/math.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <array>
#include <cmath>

static float RainHash01(int Index, int Salt)
{
	unsigned Value = (unsigned)Index * 747796405u + (unsigned)Salt * 2891336453u + 277803737u;
	Value = (Value >> ((Value >> 28u) + 4u)) ^ Value;
	Value *= 277803737u;
	Value ^= Value >> 22u;
	return (Value & 0x00ffffffu) / 16777215.0f;
}

void CRainOverlay::OnRender()
{
	if(!g_Config.m_TcRainVisual)
		return;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(GameClient()->m_Menus.IsActive())
		return;

	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	m_Time += DeltaTime * (g_Config.m_TcRainSpeed / 100.0f);

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	float OldScreenX0, OldScreenY0, OldScreenX1, OldScreenY1;
	Graphics()->GetScreen(&OldScreenX0, &OldScreenY0, &OldScreenX1, &OldScreenY1);
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);
	Graphics()->TextureClear();

	const int Amount = std::clamp(20 + g_Config.m_TcRainAmount * 2, 20, 420);
	const float Strength = std::clamp(g_Config.m_TcRainStrength / 100.0f, 0.01f, 1.0f);
	const float StreakLength = 5.5f + Strength * 8.5f;
	const float StreakWidth = 0.22f + Strength * 0.62f;
	const float AlphaBase = 0.035f + Strength * 0.20f;
	const float TravelX = ScreenWidth + 80.0f;
	const float TravelY = ScreenHeight + 90.0f;
	const float Speed = 115.0f + Strength * 145.0f;
	const vec2 Direction = normalize(vec2(-0.54f, 1.0f));
	const vec2 Normal = normalize(vec2(-Direction.y, Direction.x));

	std::array<IGraphics::CFreeformItem, 128> aItems;
	int NumItems = 0;
	Graphics()->QuadsBegin();
	for(int i = 0; i < Amount; ++i)
	{
		const float SeedX = RainHash01(i, 11);
		const float SeedY = RainHash01(i, 31);
		const float SeedSpeed = 0.70f + RainHash01(i, 57) * 0.75f;
		const float SeedAlpha = 0.45f + RainHash01(i, 83) * 0.55f;
		const float Phase = m_Time * Speed * SeedSpeed;
		float X = std::fmod(SeedX * TravelX - Phase * 0.54f, TravelX);
		float Y = std::fmod(SeedY * TravelY + Phase, TravelY);
		if(X < 0.0f)
			X += TravelX;
		if(Y < 0.0f)
			Y += TravelY;
		X -= 40.0f;
		Y -= 45.0f;

		const float Length = StreakLength * (0.70f + RainHash01(i, 101) * 0.55f);
		const float Width = StreakWidth * (0.72f + RainHash01(i, 127) * 0.42f);
		const vec2 Center = vec2(X, Y);
		const vec2 HalfLength = Direction * (Length * 0.5f);
		const vec2 HalfWidth = Normal * (Width * 0.5f);
		aItems[NumItems++] = IGraphics::CFreeformItem(
			Center - HalfLength - HalfWidth,
			Center - HalfLength + HalfWidth,
			Center + HalfLength + HalfWidth,
			Center + HalfLength - HalfWidth);

		if(NumItems == (int)aItems.size() || i == Amount - 1)
		{
			const float BatchAlpha = AlphaBase * SeedAlpha;
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, BatchAlpha);
			Graphics()->QuadsDrawFreeform(aItems.data(), NumItems);
			NumItems = 0;
		}
	}
	Graphics()->QuadsEnd();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	Graphics()->MapScreen(OldScreenX0, OldScreenY0, OldScreenX1, OldScreenY1);
}
