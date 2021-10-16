/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENT_H
#define GAME_CLIENT_COMPONENT_H

#if defined(CONF_VIDEORECORDER)
#include <engine/shared/video.h>
#endif

#include <base/color.h>
#include <engine/input.h>

#include <engine/client.h>
#include <engine/console.h>
#include <game/localization.h>

#include <engine/config.h>

class CGameClient;

enum EComponentMouseMovementBlockMode
{
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_DONT_BLOCK = 0,
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_BLOCK,
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_BLOCK_AND_CHANGE_TO_INGAME,
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_BLOCK_AND_CHANGE_TO_INGAME_RELATIVE,
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_BLOCK_AND_CHANGE_TO_RELATIVE,
	COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_BLOCK_AND_CHANGE_TO_ABSOLUTE,
};

/**
* This class is inherited by all the client components.
*
* These components can implement the virtual methods such as OnInit(), OnMessage(int Msg, void *pRawMsg) to provide their functionality.
*/
class CComponent
{
protected:
	friend class CGameClient;

	CGameClient *m_pClient;

	// perhaps propagate pointers for these as well

	/**
	 * Get the kernel interface.
	 */
	class IKernel *Kernel() const;
	/**
	 * Get the graphics interface.
	 */
	class IGraphics *Graphics() const;
	/**
	 * Get the text rendering interface.
	 */
	class ITextRender *TextRender() const;
	/**
	 * Get the input interface.
	 */
	class IInput *Input() const;
	/**
	 * Get the storage interface.
	 */
	class IStorage *Storage() const;
	/**
	 * Get the ui interface.
	 */
	class CUI *UI() const;
	/**
	 * Get the sound interface.
	 */
	class ISound *Sound() const;
	/**
	 * Get the render tools interface.
	 */
	class CRenderTools *RenderTools() const;
	/**
	 * Get the config interface.
	 */
	class CConfig *Config() const;
	/**
	 * Get the console interface.
	 */
	class IConsole *Console() const;
	/**
	 * Get the demo player interface.
	 */
	class IDemoPlayer *DemoPlayer() const;
	/**
	 * Get the demo recorder interface.
	 *
	 * @param Recorder A member of the RECORDER_x enum
	 * @see RECORDER_MANUAL
	 * @see RECORDER_AUTO
	 * @see RECORDER_RACE
	 * @see RECORDER_REPLAYS
	 */
	class IDemoRecorder *DemoRecorder(int Recorder) const;
	/**
	 * Get the server browser interface.
	 */
	class IServerBrowser *ServerBrowser() const;
	/**
	 * Get the layers interface.
	 */
	class CLayers *Layers() const;
	/**
	 * Get the collision interface.
	 */
	class CCollision *Collision() const;
#if defined(CONF_AUTOUPDATE)
	/**
	 * Get the updater interface.
	 */
	class IUpdater *Updater() const;
#endif

#if defined(CONF_VIDEORECORDER)
	/**
	 * Gets the current time.
	 * @see time_get()
	 */
	int64_t time() const
	{
		return IVideo::Current() ? IVideo::Time() : time_get();
	}
#else
	/**
	 * Gets the current time.
	 * @see time_get()
	 */
	int64_t time() const
	{
		return time_get();
	}
#endif
	/**
	 * Gets the local time.
	 */
	float LocalTime() const;

public:
	/**
	 * The component virtual destructor.
	 */
	virtual ~CComponent() {}
	/**
	 * Get a pointer to the game client.
	 */
	class CGameClient *GameClient() const { return m_pClient; }
	/**
	 * Get the client interface.
	 */
	class IClient *Client() const;

	/**
	 * This method is called when the client changes state, e.g from offline to online.
	 * @see IClient::STATE_CONNECTING
	 * @see IClient::STATE_LOADING
	 * @see IClient::STATE_ONLINE
	 */
	virtual void OnStateChange(int NewState, int OldState){};
	/**
	 * Called to let the components register their console commands.
	 */
	virtual void OnConsoleInit(){};
	/**
	 * Called to let the components run initialization code.
	 */
	virtual void OnInit(){};
	/**
	 * Called to reset the component.
	 * This method is usually called on your component constructor to avoid code duplication.
	 * @see CHud::CHud()
	 * @see CHud::OnReset()
	 */
	virtual void OnReset(){};
	/**
	 * Called when the window has been resized.
	 */
	virtual void OnWindowResize() {}
	/**
	 * Called when the component should get rendered.
	 *
	 * The render order depends on the component insertion order.
	 */
	virtual void OnRender(){};
	/**
	 * Called when the input gets released, for example when a text box loses focus.
	 */
	virtual void OnRelease(){};
	/**
	 * Called on map load.
	 */
	virtual void OnMapLoad(){};
	/**
	 * Called when receiving a network message.
	 * @param Msg The message type.
	 * @param pRawMsg The message data.
	 * @see NETMSGTYPE_SV_DDRACETIME
	 * @see CNetMsg_Sv_DDRaceTime
	 */
	virtual void OnMessage(int Msg, void *pRawMsg) {}
	/**
	 * Called on mouse movement, where the x and y values are the desktop cursor coordinates relative to the window rect.
	 *
	 * @param x The x relative coordinate of the desktop cursor inside the window rect.
	 * @param y The y relative coordinate of the desktop cursor inside the window rect.
	 * @return Returns how to block the mouse for components that are called after the current component. Can also be used to change the mouse mode to a desired mode.
	 */
	virtual EComponentMouseMovementBlockMode OnMouseInWindowPos(int X, int Y) { return COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_DONT_BLOCK; }
	/**
	 * Called on absolute mouse movement, where the x and y values are the desktop cursor coordinates relative to the window rect.
	 * It's similar to @see OnMouseInWindowPos, but does not grab the mouse inside the window and also shows the desktop cursor
	 *
	 * @param x The x relative coordinate of the desktop cursor inside the window rect.
	 * @param y The y relative coordinate of the desktop cursor inside the window rect.
	 * @return Returns how to block the mouse for components that are called after the current component. Can also be used to change the mouse mode to a desired mode.
	 */
	virtual EComponentMouseMovementBlockMode OnMouseAbsoluteInWindowPos(int X, int Y) { return COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_DONT_BLOCK; }
	/**
	 * Called on relative mouse movement, where the x and y values are deltas of the desktop cursor.
	 *
	 * @param x The amount of change in the x coordinate since the last call.
	 * @param y The amount of change in the y coordinate since the last call.
	 * @return Returns how to block the mouse for components that are called after the current component. Can also be used to change the mouse mode to a desired mode.
	 */
	virtual EComponentMouseMovementBlockMode OnMouseInWindowRelativeMove(int X, int Y) { return COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_DONT_BLOCK; }
	/**
	 * Called on relative mouse movement, where the x and y values are deltas.
	 *
	 * @param x The amount of change in the x coordinate since the last call.
	 * @param y The amount of change in the y coordinate since the last call.
	 * @return Returns how to block the mouse for components that are called after the current component. Can also be used to change the mouse mode to a desired mode.
	 */
	virtual EComponentMouseMovementBlockMode OnMouseRelativeMove(float x, float y) { return COMPONENT_MOUSE_MOVEMENT_BLOCK_MODE_DONT_BLOCK; }
	/**
	 * Called on a input event.
	 * @param e The input event.
	 */
	virtual bool OnInput(IInput::CEvent e) { return false; }
};

#endif
