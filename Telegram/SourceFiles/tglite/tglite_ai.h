/*
Plume: ИИ-помощник на компьютере. Модель — через сервер plume.amdiveev.ru (/api/ai),
действия (чаты, папки, сообщения, рассылка) выполняются здесь, сессия Telegram никуда не уходит.
Licensed under GNU GPL v. 3.
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class AbstractButton;
class RpWidget;
class InputField;
} // namespace Ui

class QWidget;
class History;
class HistoryItem;
class PeerData;
class UserData;

namespace TgLite {

void InstallReplySuggestions(
	not_null<Ui::InputField*> field,
	not_null<Window::SessionController*> controller,
	Fn<History*()> currentHistory);

void ShowAssistant(not_null<Window::SessionController*> controller);
void ToggleAssistant(not_null<Window::SessionController*> controller); // Ctrl+T и кнопка «Takt»

// Takt из открытого чата: знает chat_id, название и (если выбрано) сообщение; в панели — кнопки частых просьб.
void ShowAssistantForChat(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId msgId,
	const QString &text);

// Кнопка «Takt» в шапке открытого чата.
[[nodiscard]] Ui::AbstractButton *CreateChatTaktButton(
	not_null<QWidget*> parent,
	Fn<void()> click);

// Кнопка «Takt» со счётчиком «ждут вас» (справа от поиска). Ставит Ctrl+T и открывает Takt при старте, если он был открыт.
[[nodiscard]] Ui::AbstractButton *CreateTaktButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// Быстрые разделы Плюма над папками, отдельно от меню Telegram
[[nodiscard]] Ui::RpWidget *CreatePlumeNavigation(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// Круглая кнопка ↻ «Обновить» в шапке: новая версия Плюма в одно нажатие
[[nodiscard]] Ui::AbstractButton *CreateRefreshButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// Маяк внутри Плюма: мини-приложение бота @mayak52_bot (mayak.amdiveev.ru, вход по Telegram без пароля).
void OpenMayak(not_null<Window::SessionController*> controller);
// «В Маяк» у сообщения: новый чат в Маяке с этим сообщением (и его фото/файлом) — сообщением «чат: …» своему боту.
void ShowSendToMayak(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);
// Бот Маяка, если он уже загружен; иначе nullptr (и подгружает его на следующий раз).
[[nodiscard]] UserData *MayakBot(not_null<Main::Session*> session);
// Круглая кнопка-маяк рядом с «Помощником» над чатами.
[[nodiscard]] Ui::AbstractButton *CreateMayakButton(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller);

// «Взаимодействие по броням»: в отмеченном личном чате Плюм на сервере сам переписывается о брони залов НКО
// (yavd.ru) от аккаунта владельца. Список чатов — на сервере (plume.amdiveev.ru/api/bron), пункт меню чата —
// только в том аккаунте, в который вошёл сервер, и только для людей (не боты, не группы).
[[nodiscard]] bool BronAvailable(not_null<PeerData*> peer);
[[nodiscard]] bool IsBron(not_null<PeerData*> peer);
// Подпись «✓ брони» у чата в списке и в шапке — только там, где Плюм ведёт брони от этого аккаунта.
[[nodiscard]] bool BronShown(not_null<PeerData*> peer);
[[nodiscard]] QString BronMark();
void SetBron(not_null<Window::SessionController*> controller, not_null<PeerData*> peer, bool on);
void RefreshBron();

// Раз в 30 с забирает задания других проектов (plume.amdiveev.ru/api/cmd-pending) и выполняет их помощником.
void StartCommandPolling();

// Менеджер аккаунтов (plume.amdiveev.ru/accounts): какие аккаунты открыты в этом Плюме —
// имя, @имя, телефон, «скрыт». Сессии никуда не уходят. Сам — при запуске и раз в 30 минут.
void ReportAccounts();

// Общие настройки Плюма: забирает с сервера при запуске и раз в 3 минуты, отправляет после изменения.
void PullSettings();
void PushSettings();

} // namespace TgLite
