/*
Plume — свои функции поверх Telegram Desktop: встроенный VPN по подписке StealthSurf
(xray рядом с программой + обычный прокси Telegram) и обновления со своего сервера.
Licensed under GNU GPL v. 3.
*/
#pragma once

class History;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace TgLite {

// Номер сборки (ггммддчч); подставляет скрипт сборки. По нему работает обновление.
inline constexpr auto kBuild = 26102312;

void Start();
void Stop();
void CheckUpdates(bool manual);
[[nodiscard]] bool HideStories(); // по умолчанию да, меняется в окне Plume, действует после перезапуска
[[nodiscard]] bool HideRecentSearch(); // по умолчанию да: в пустом поиске нет «Недавних» и частых собеседников
// Архив без мусора: чужие каналы и открытые группы в архиве не показываются (по умолчанию да).
[[nodiscard]] bool HideArchiveJunk();
[[nodiscard]] bool ArchiveJunkHidden(not_null<const History*> history);
void SetHideArchiveJunk(not_null<Main::Session*> session, bool enabled);
void ShowSettingsBox(not_null<Window::SessionController*> controller);

// Фокус-режим: для всех «не в сети», выбранные личные чаты спрятаны из списка и без уведомлений.
// Все уведомления выключены одним переключателем (☰ и Настройки Плюма).
[[nodiscard]] bool MuteAll();
[[nodiscard]] rpl::producer<bool> MuteAllValue();
void SetMuteAll(not_null<Window::SessionController*> controller, bool enabled);
[[nodiscard]] bool FocusActive();
[[nodiscard]] bool FocusHidden(not_null<const History*> history);
[[nodiscard]] rpl::producer<bool> FocusValue();
void SetFocus(not_null<Window::SessionController*> controller, bool enabled);
void ShowFocusBox(not_null<Window::SessionController*> controller);
[[nodiscard]] bool GhostActive();
[[nodiscard]] rpl::producer<bool> GhostValue();
void SetGhost(not_null<Window::SessionController*> controller, bool enabled);

} // namespace TgLite
