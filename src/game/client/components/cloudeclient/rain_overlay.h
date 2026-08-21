#ifndef GAME_CLIENT_COMPONENTS_CLOUDECLIENT_RAIN_OVERLAY_H
#define GAME_CLIENT_COMPONENTS_CLOUDECLIENT_RAIN_OVERLAY_H

#include <game/client/component.h>

class CRainOverlay : public CComponent
{
	float m_Time = 0.0f;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;
};

#endif // GAME_CLIENT_COMPONENTS_CLOUDECLIENT_RAIN_OVERLAY_H
