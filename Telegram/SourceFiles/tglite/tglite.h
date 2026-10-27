/*
Plume — свои функции поверх Telegram Desktop: встроенный VPN по подписке StealthSurf
(xray рядом с программой + обычный прокси Telegram) и обновления со своего сервера.
Licensed under GNU GPL v. 3.
*/
#pragma once

#include <QtCore/QJsonObject>

class History;

namespace Main {
class Account;
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace TgLite {

// Номер сборки (ггммддчч); подставляет скрипт сборки. По нему работает обновление.
inline constexpr auto kBuild = 26102723;

void Start();
void Stop();
void CheckUpdates(bool manual);
[[nodiscard]] bool HideStories(); // по умолчанию да, меняется в окне Plume, действует после перезапуска
[[nodiscard]] bool HideRecentSearch(); // по умолчанию да: в пустом поиске нет «Недавних» и частых собеседников
// Архив без мусора: чужие каналы и открытые группы в архиве не показываются (по умолчанию да).
[[nodiscard]] bool HideArchiveJunk();
[[nodiscard]] bool ArchiveJunkHidden(not_null<const History*> history);
void SetHideArchiveJunk(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] bool HideTaktBot();
[[nodiscard]] bool TaktBotHidden(not_null<const History*> history);
void SetHideTaktBot(not_null<Main::Session*> session, bool enabled);
// Бот Маяка (@mayak52_bot) спрятан из списка чатов: Маяк — кнопкой рядом с Помощником. Уведомления от него приходят.
[[nodiscard]] bool HideMayakBot();
[[nodiscard]] bool MayakBotHidden(not_null<const History*> history);
void SetHideMayakBot(not_null<Main::Session*> session, bool enabled);
// Чужие боты в архиве тоже не показываются (по умолчанию да; свои и бот Такт остаются).
[[nodiscard]] bool HideArchiveBots();
void SetHideArchiveBots(not_null<Main::Session*> session, bool enabled);
// Тихие группы: больше 30 непрочитанных, вы не админ — без уведомлений, кроме упоминаний (по умолчанию да).
[[nodiscard]] bool QuietGroups();
[[nodiscard]] bool QuietGroup(not_null<const History*> history);
void SetQuietGroups(bool enabled);
// Сводка: непрочитанные, кто шумит, что спрятано, и выход из заброшенных каналов и групп.
void ShowSummaryBox(not_null<Window::SessionController*> controller);

// Общие настройки на телефоне, ПК и Mac (plume.amdiveev.ru/api/settings/<id>, синхронизация — tglite_ai.cpp).
[[nodiscard]] QJsonObject SyncedSettings();
[[nodiscard]] qint64 SettingsUpdated();
void SetSettingsUpdated(qint64 updated);
bool ApplySyncedSettings(const QJsonObject &settings, qint64 updated);
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

// Скрытые аккаунты (до 30 аккаунтов в Плюме): не видны в списке аккаунтов, кроме открытого,
// без уведомлений и счётчика непрочитанных. tglite.json → hiddenAccounts (id пользователей).
[[nodiscard]] bool AccountHidden(not_null<Main::Account*> account);
[[nodiscard]] rpl::producer<> HiddenAccountsChanges();
void SetAccountHidden(not_null<Main::Account*> account, bool hidden);
void ShowHiddenAccountsBox(not_null<Window::SessionController*> controller);

} // namespace TgLite
