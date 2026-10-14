/*
Plume (TG Lite): ИИ-помощник на компьютере.
Licensed under GNU GPL v. 3.

Цикл как в веб-версии (src/lite/ai/aiAgent.ts в m0t1v3131/tg-lite): сервер /api/ai только передаёт
разговор модели (формат Anthropic Messages, tool_use), инструменты выполняются здесь.
Рассылка — с теми же ограничениями: ≤25 адресатов за раз, ≤60 в сутки, пауза 25–60 с, всегда подтверждение.
*/
#include "tglite/tglite_ai.h"

#include "apiwrap.h"
#include "core/application.h"
#include "window/window_controller.h"
#include "api/api_common.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDate>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TgLite {
namespace {

constexpr auto kEndpoint = "https://tg.amdiveev.ru/api/ai";
constexpr auto kStepsMax = 15;
constexpr auto kBroadcastOnce = 25;
constexpr auto kBroadcastDaily = 60;

QString StatePath() {
	return cWorkingDir() + u"tglite.json"_q;
}

QJsonObject ReadState() {
	QFile f(StatePath());
	return f.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(f.readAll()).object()
		: QJsonObject();
}

void SetState(const QString &key, const QJsonValue &value) {
	auto state = ReadState();
	state.insert(key, value);
	QFile f(StatePath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(QJsonDocument(state).toJson());
	}
}

QNetworkAccessManager *Net() {
	static auto result = new QNetworkAccessManager(QCoreApplication::instance());
	return result;
}

QJsonObject Tool(
		const QString &name,
		const QString &description,
		QJsonObject properties,
		QStringList required = {}) {
	return QJsonObject{
		{ u"name"_q, name },
		{ u"description"_q, description },
		{ u"input_schema"_q, QJsonObject{
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, properties },
			{ u"required"_q, QJsonArray::fromStringList(required) },
		} },
	};
}

QJsonObject Prop(const QString &type, const QString &description) {
	return QJsonObject{ { u"type"_q, type }, { u"description"_q, description } };
}

QJsonObject IdsProp(const QString &description) {
	return QJsonObject{
		{ u"type"_q, u"array"_q },
		{ u"items"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
		{ u"description"_q, description },
	};
}

QJsonArray Tools() {
	return QJsonArray{
		Tool(u"list_chats"_q,
			u"Список чатов (включая архив): id, название, тип, непрочитанные. Можно искать по части названия."_q,
			{ { u"query"_q, Prop(u"string"_q, u"часть названия, необязательно"_q) },
			  { u"limit"_q, Prop(u"integer"_q, u"сколько вернуть, по умолчанию 50"_q) } }),
		Tool(u"list_folders"_q,
			u"Список папок: id, название, сколько чатов добавлено вручную."_q,
			{}),
		Tool(u"create_folder"_q,
			u"Создать папку с выбранными чатами."_q,
			{ { u"title"_q, Prop(u"string"_q, u"название папки"_q) },
			  { u"chat_ids"_q, IdsProp(u"id чатов из list_chats"_q) } },
			{ u"title"_q, u"chat_ids"_q }),
		Tool(u"edit_folder"_q,
			u"Добавить чаты в папку или убрать из неё."_q,
			{ { u"folder_id"_q, Prop(u"integer"_q, u"id папки из list_folders"_q) },
			  { u"add_chat_ids"_q, IdsProp(u"кого добавить"_q) },
			  { u"remove_chat_ids"_q, IdsProp(u"кого убрать"_q) },
			  { u"title"_q, Prop(u"string"_q, u"новое название, необязательно"_q) } },
			{ u"folder_id"_q }),
		Tool(u"read_messages"_q,
			u"Последние сообщения чата (до 50)."_q,
			{ { u"chat_id"_q, Prop(u"string"_q, u"id чата"_q) },
			  { u"limit"_q, Prop(u"integer"_q, u"сколько, по умолчанию 20"_q) } },
			{ u"chat_id"_q }),
		Tool(u"send_message"_q,
			u"Отправить сообщение в один чат (пользователь подтверждает)."_q,
			{ { u"chat_id"_q, Prop(u"string"_q, u"id чата"_q) },
			  { u"text"_q, Prop(u"string"_q, u"текст"_q) } },
			{ u"chat_id"_q, u"text"_q }),
		Tool(u"broadcast"_q,
			u"Разослать одно сообщение в несколько чатов (≤25 за раз, ≤60 в сутки, паузы 25–60 с; пользователь подтверждает). Только чаты, где уже есть переписка или где пользователь участник."_q,
			{ { u"chat_ids"_q, IdsProp(u"id чатов"_q) },
			  { u"text"_q, Prop(u"string"_q, u"текст"_q) } },
			{ u"chat_ids"_q, u"text"_q }),
	};
}

QString SystemPrompt() {
	return u"Ты — помощник внутри Plume, личного Telegram-клиента Александра. "
		u"Отвечай по-русски, коротко и просто. Делай то, что просят, с помощью инструментов: "
		u"находи чаты через list_chats, папки — list_folders, создавай и меняй папки, читай сообщения, отправляй. "
		u"Перед отправкой и рассылкой клиент сам спросит подтверждение — не переспрашивай лишний раз. "
		u"Рассылки только небольшие (до 25 чатов), это личный аккаунт — массовые рассылки могут привести к блокировке."_q;
}

QString PeerType(not_null<PeerData*> peer) {
	if (peer->isUser()) {
		return peer->asUser()->isBot() ? u"бот"_q : u"личный"_q;
	} else if (peer->isBroadcast()) {
		return u"канал"_q;
	}
	return u"группа"_q;
}

std::vector<not_null<History*>> AllChats(not_null<Main::Session*> session) {
	auto result = std::vector<not_null<History*>>();
	const auto append = [&](not_null<Dialogs::IndexedList*> list) {
		for (const auto &row : list->all()) {
			if (const auto history = row->history()) {
				result.push_back(history);
			}
		}
	};
	append(session->data().chatsList()->indexed());
	if (const auto folder = session->data().folderLoaded(Data::Folder::kId)) {
		append(folder->chatsList()->indexed());
	}
	return result;
}

History *FindChat(not_null<Main::Session*> session, const QString &id) {
	auto ok = false;
	const auto value = id.toULongLong(&ok);
	return ok ? session->data().historyLoaded(PeerId(PeerIdHelper(value))) : nullptr;
}

QString ChatId(not_null<History*> history) {
	return QString::number(history->peer->id.value);
}

class Assistant final : public base::has_weak_ptr {
public:
	explicit Assistant(not_null<Window::SessionController*> controller)
	: _controller(controller)
	, _session(&controller->session()) {
	}

	void attach(not_null<Ui::FlatLabel*> feed) {
		_feed = feed.get();
		render();
	}

	// Задание из другого проекта (очередь /api/cmd): тот же цикл, итог — в done.
	void command(const QString &from, const QString &text, Fn<void(QString, bool)> done) {
		_onDone = std::move(done);
		_lastText = QString();
		add(u"Задание от "_q + from + u": "_q + text.trimmed());
		_messages.append(QJsonObject{
			{ u"role"_q, u"user"_q },
			{ u"content"_q, u"[Задание пришло из проекта «"_q + from
				+ u"», пользователь может быть не у компьютера. Выполни и коротко отчитайся.] "_q
				+ text.trimmed() },
		});
		_steps = 0;
		step();
	}

	void ask(const QString &text) {
		if (_busy || text.trimmed().isEmpty()) {
			return;
		}
		add(u"Вы: "_q + text.trimmed());
		_messages.append(QJsonObject{
			{ u"role"_q, u"user"_q },
			{ u"content"_q, text.trimmed() },
		});
		_steps = 0;
		step();
	}

	[[nodiscard]] bool busy() const {
		return _busy;
	}

private:
	void add(const QString &line) {
		_lines.push_back(line);
		while (_lines.size() > 60) {
			_lines.pop_front();
		}
		render();
	}

	void render() {
		if (_feed) {
			_feed->setText(_lines.isEmpty()
				? u"Напишите, что сделать: например «создай папку Штаб и добавь туда все чаты со словом 195» или «напиши Оле, что встреча в 18:00»."_q
				: _lines.join(u"\n\n"_q));
		}
	}

	void step() {
		if (++_steps > kStepsMax) {
			add(u"Остановился: слишком много шагов."_q);
			finish(u"остановился: слишком много шагов"_q, false);
			return;
		}
		_busy = true;
		const auto key = ReadState().value(u"aiKey"_q).toString();
		auto request = QNetworkRequest(QUrl(QString(kEndpoint)));
		request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
		request.setRawHeader("X-Lite-Key", key.toUtf8());
		request.setTransferTimeout(120000);
		const auto body = QJsonDocument(QJsonObject{
			{ u"system"_q, SystemPrompt() },
			{ u"messages"_q, _messages },
			{ u"tools"_q, Tools() },
		}).toJson(QJsonDocument::Compact);
		const auto reply = Net()->post(request, body);
		QObject::connect(reply, &QNetworkReply::finished, [=, weak = base::make_weak(this)] {
			reply->deleteLater();
			if (!weak) {
				return;
			}
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const auto data = QJsonDocument::fromJson(reply->readAll()).object();
			if (status == 401) {
				SetState(u"aiKey"_q, QString());
				add(u"Ключ доступа не подошёл — закройте окно и введите его заново."_q);
				finish(u"ключ доступа не подошёл"_q, false);
				return;
			} else if (reply->error() != QNetworkReply::NoError || data.isEmpty()) {
				add(u"Нет связи с помощником: "_q + reply->errorString());
				finish(u"нет связи с помощником"_q, false);
				return;
			}
			handle(data);
		});
	}

	void handle(const QJsonObject &data) {
		const auto content = data.value(u"content"_q).toArray();
		_messages.append(QJsonObject{
			{ u"role"_q, u"assistant"_q },
			{ u"content"_q, content },
		});
		auto calls = std::vector<QJsonObject>();
		for (const auto &block : content) {
			const auto o = block.toObject();
			const auto type = o.value(u"type"_q).toString();
			if (type == u"text"_q) {
				const auto text = o.value(u"text"_q).toString().trimmed();
				if (!text.isEmpty()) {
					add(u"Plume: "_q + text);
					_lastText = text;
				}
			} else if (type == u"tool_use"_q) {
				calls.push_back(o);
			}
		}
		if (calls.empty()) {
			finish(_lastText.isEmpty() ? u"готово"_q : _lastText, true);
			return;
		}
		_results = QJsonArray();
		runCalls(std::move(calls), 0);
	}

	void runCalls(std::vector<QJsonObject> calls, int index) {
		if (index >= int(calls.size())) {
			_messages.append(QJsonObject{
				{ u"role"_q, u"user"_q },
				{ u"content"_q, _results },
			});
			step();
			return;
		}
		const auto call = calls[index];
		const auto id = call.value(u"id"_q).toString();
		run(
			call.value(u"name"_q).toString(),
			call.value(u"input"_q).toObject(),
			[=, weak = base::make_weak(this)](QString result, bool error) {
				if (!weak) {
					return;
				}
				_results.append(QJsonObject{
					{ u"type"_q, u"tool_result"_q },
					{ u"tool_use_id"_q, id },
					{ u"content"_q, result.left(12000) },
					{ u"is_error"_q, error },
				});
				runCalls(calls, index + 1);
			});
	}

	void confirm(const QString &text, Fn<void()> yes, Fn<void()> no) {
		_controller->show(Ui::MakeConfirmBox({
			.text = text,
			.confirmed = [=](Fn<void()> close) { close(); yes(); },
			.cancelled = [=](Fn<void()> close) { close(); no(); },
			.confirmText = u"Выполнить"_q,
			.cancelText = u"Отмена"_q,
		}));
	}

	void run(
			const QString &name,
			const QJsonObject &input,
			Fn<void(QString, bool)> done) {
		const auto ids = [&](const QString &key) {
			auto result = std::vector<not_null<History*>>();
			for (const auto &v : input.value(key).toArray()) {
				if (const auto history = FindChat(_session, v.toString())) {
					result.push_back(history);
				}
			}
			return result;
		};
		if (name == u"list_chats"_q) {
			add(u"· смотрю чаты"_q);
			const auto query = input.value(u"query"_q).toString().toLower();
			const auto limit = std::clamp(input.value(u"limit"_q).toInt(50), 1, 300);
			auto lines = QStringList();
			for (const auto history : AllChats(_session)) {
				const auto title = history->peer->name();
				if (!query.isEmpty() && !title.toLower().contains(query)) {
					continue;
				}
				lines.push_back(ChatId(history) + u" | "_q + title + u" | "_q
					+ PeerType(history->peer) + u" | непрочитано "_q
					+ QString::number(history->unreadCount()));
				if (lines.size() >= limit) {
					break;
				}
			}
			done(lines.isEmpty() ? u"ничего не нашлось"_q : lines.join('\n'), false);
		} else if (name == u"list_folders"_q) {
			add(u"· смотрю папки"_q);
			auto lines = QStringList();
			for (const auto &filter : _session->data().chatsFilters().list()) {
				if (!filter.id()) {
					continue;
				}
				lines.push_back(QString::number(filter.id()) + u" | "_q
					+ filter.titleText().text + u" | чатов вручную: "_q
					+ QString::number(filter.always().size()));
			}
			done(lines.isEmpty() ? u"папок нет"_q : lines.join('\n'), false);
		} else if (name == u"create_folder"_q) {
			const auto title = input.value(u"title"_q).toString().trimmed();
			const auto chats = ids(u"chat_ids"_q);
			if (title.isEmpty() || chats.empty()) {
				done(u"нужны название и хотя бы один чат"_q, true);
				return;
			}
			add(u"· создаю папку «"_q + title + u"»"_q);
			const auto &list = _session->data().chatsFilters().list();
			auto id = FilterId(2);
			while (ranges::contains(list, id, &Data::ChatFilter::id)) {
				++id;
			}
			auto always = base::flat_set<not_null<History*>>();
			for (const auto history : chats) {
				always.emplace(history);
			}
			const auto filter = Data::ChatFilter(
				id,
				Data::ChatFilterTitle{ TextWithEntities{ title } },
				QString(),
				std::nullopt,
				Data::ChatFilter::Flags(),
				std::move(always),
				{},
				{});
			saveFilter(filter, done, u"папка создана, id "_q + QString::number(id));
		} else if (name == u"edit_folder"_q) {
			const auto folderId = input.value(u"folder_id"_q).toInt();
			const auto &list = _session->data().chatsFilters().list();
			const auto i = ranges::find(list, folderId, &Data::ChatFilter::id);
			if (!folderId || i == end(list)) {
				done(u"нет такой папки"_q, true);
				return;
			}
			add(u"· меняю папку «"_q + i->titleText().text + u"»"_q);
			auto always = i->always();
			auto never = i->never();
			for (const auto history : ids(u"add_chat_ids"_q)) {
				always.emplace(history);
				never.remove(history);
			}
			for (const auto history : ids(u"remove_chat_ids"_q)) {
				always.remove(history);
			}
			const auto title = input.value(u"title"_q).toString().trimmed();
			const auto filter = Data::ChatFilter(
				i->id(),
				title.isEmpty()
					? i->title()
					: Data::ChatFilterTitle{ TextWithEntities{ title } },
				i->iconEmoji(),
				i->colorIndex(),
				i->flags(),
				std::move(always),
				i->pinned(),
				std::move(never));
			saveFilter(filter, done, u"папка изменена"_q);
		} else if (name == u"read_messages"_q) {
			const auto history = FindChat(_session, input.value(u"chat_id"_q).toString());
			if (!history) {
				done(u"нет такого чата"_q, true);
				return;
			}
			add(u"· читаю «"_q + history->peer->name() + u"»"_q);
			readMessages(history, std::clamp(input.value(u"limit"_q).toInt(20), 1, 50), done);
		} else if (name == u"send_message"_q) {
			const auto history = FindChat(_session, input.value(u"chat_id"_q).toString());
			const auto text = input.value(u"text"_q).toString();
			if (!history || text.trimmed().isEmpty()) {
				done(u"нет такого чата или пустой текст"_q, true);
				return;
			}
			confirm(
				u"Отправить в «"_q + history->peer->name() + u"»:\n\n"_q + text,
				[=] {
					send(history, text);
					add(u"· отправлено в «"_q + history->peer->name() + u"»"_q);
					done(u"отправлено"_q, false);
				},
				[=] { done(u"пользователь отменил"_q, false); });
		} else if (name == u"broadcast"_q) {
			broadcast(ids(u"chat_ids"_q), input.value(u"text"_q).toString(), done);
		} else {
			done(u"неизвестный инструмент"_q, true);
		}
	}

	void saveFilter(
			const Data::ChatFilter &filter,
			Fn<void(QString, bool)> done,
			const QString &okText) {
		_session->data().chatsFilters().set(filter);
		_session->api().request(MTPmessages_UpdateDialogFilter(
			MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
			MTP_int(filter.id()),
			filter.tl()
		)).done([=] {
			done(okText, false);
		}).fail([=](const MTP::Error &error) {
			_session->data().chatsFilters().reload();
			done(u"Telegram отказал: "_q + error.type(), true);
		}).send();
	}

	void readMessages(
			not_null<History*> history,
			int limit,
			Fn<void(QString, bool)> done) {
		const auto session = _session;
		session->api().request(MTPmessages_GetHistory(
			history->peer->input(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done([=](const MTPmessages_Messages &result) {
			auto lines = QStringList();
			result.match([](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				for (const auto &message : data.vmessages().v) {
					message.match([&](const MTPDmessage &m) {
						auto from = QString();
						if (const auto fromId = m.vfrom_id()) {
							if (const auto peer = session->data().peerLoaded(peerFromMTP(*fromId))) {
								from = peer->name();
							}
						} else if (m.is_out()) {
							from = u"я"_q;
						}
						const auto date = QDateTime::fromSecsSinceEpoch(m.vdate().v);
						lines.push_front(date.toString(u"dd.MM HH:mm"_q) + u" "_q
							+ (from.isEmpty() ? history->peer->name() : from)
							+ u": "_q + qs(m.vmessage()));
					}, [](const auto &) {});
				}
			});
			done(lines.isEmpty() ? u"сообщений нет"_q : lines.join('\n'), false);
		}).fail([=](const MTP::Error &error) {
			done(u"не удалось прочитать: "_q + error.type(), true);
		}).send();
	}

	void send(not_null<History*> history, const QString &text) {
		auto message = Api::MessageToSend(Api::SendAction(history));
		message.textWithTags = { text, TextWithTags::Tags() };
		_session->api().sendMessage(std::move(message));
	}

	void broadcast(
			std::vector<not_null<History*>> chats,
			const QString &text,
			Fn<void(QString, bool)> done) {
		if (chats.empty() || text.trimmed().isEmpty()) {
			done(u"нет чатов или пустой текст"_q, true);
			return;
		}
		if (int(chats.size()) > kBroadcastOnce) {
			done(u"за раз можно не больше 25 чатов — это защита аккаунта от блокировки"_q, true);
			return;
		}
		const auto today = QDate::currentDate().toString(Qt::ISODate);
		auto state = ReadState();
		const auto sentToday = (state.value(u"bcDay"_q).toString() == today)
			? state.value(u"bcCount"_q).toInt()
			: 0;
		if (sentToday + int(chats.size()) > kBroadcastDaily) {
			done(u"сегодня уже отправлено "_q + QString::number(sentToday)
				+ u", лимит 60 в сутки — защита аккаунта"_q, true);
			return;
		}
		auto names = QStringList();
		for (const auto history : chats) {
			names.push_back(history->peer->name());
		}
		const auto minutes = (int(chats.size()) * 42) / 60 + 1;
		confirm(
			u"Разослать в "_q + QString::number(chats.size()) + u" чатов (≈"_q
				+ QString::number(minutes) + u" мин, с паузами):\n"_q
				+ names.join(u", "_q) + u"\n\n"_q + text,
			[=] {
				SetState(u"bcDay"_q, today);
				SetState(u"bcCount"_q, sentToday + int(chats.size()));
				add(u"· рассылка запущена: "_q + QString::number(chats.size()) + u" чатов"_q);
				sendNext(chats, text, 0);
				done(u"рассылка запущена, идёт в фоне с паузами"_q, false);
			},
			[=] { done(u"пользователь отменил"_q, false); });
	}

	void sendNext(
			std::vector<not_null<History*>> chats,
			QString text,
			int index) {
		if (index >= int(chats.size())) {
			add(u"· рассылка завершена"_q);
			return;
		}
		send(chats[index], text);
		add(u"· "_q + QString::number(index + 1) + u"/"_q
			+ QString::number(chats.size()) + u" — «"_q
			+ chats[index]->peer->name() + u"»"_q);
		const auto delay = 25000 + int(base::RandomValue<uint32>() % 35000);
		base::call_delayed(delay, _session, [=] {
			sendNext(chats, text, index + 1);
		});
	}

	void finish(const QString &result, bool ok) {
		_busy = false;
		if (const auto done = base::take(_onDone)) {
			done(result, ok);
		}
	}

	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;
	QPointer<Ui::FlatLabel> _feed;
	QJsonArray _messages;
	QJsonArray _results;
	QStringList _lines;
	QString _lastText;
	Fn<void(QString, bool)> _onDone;
	int _steps = 0;
	bool _busy = false;

};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<Assistant>> Assistants;

not_null<Assistant*> AssistantFor(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	auto &result = Assistants[session];
	if (!result) {
		result = std::make_unique<Assistant>(controller);
		session->lifetime().add([=] { Assistants.remove(session); });
	}
	return result.get();
}

void ShowKeyBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Помощник Plume"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Введите ключ доступа к помощнику (тот же, что в веб-версии tg.amdiveev.ru). Он сохранится на этом компьютере."_q,
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Ключ доступа"_q),
			QString()));
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto key = field->getLastText().trimmed();
			if (key.isEmpty()) {
				field->showError();
				return;
			}
			SetState(u"aiKey"_q, key);
			box->closeBox();
			ShowAssistant(controller);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void PollCommands();

void FinishCommand(const QString &id, const QString &result, bool ok) {
	auto request = QNetworkRequest(QUrl(u"https://tg.amdiveev.ru/api/cmd/"_q + id + u"/done"_q));
	request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
	request.setRawHeader("X-Lite-Key", ReadState().value(u"aiKey"_q).toString().toUtf8());
	const auto reply = Net()->post(request, QJsonDocument(QJsonObject{
		{ u"result"_q, result },
		{ u"ok"_q, ok },
	}).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

bool CommandRunning = false;

void RunCommands(std::vector<QJsonObject> list, int index) {
	if (index >= int(list.size())) {
		CommandRunning = false;
		return;
	}
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	const auto item = list[index];
	const auto id = item.value(u"id"_q).toString();
	if (!controller) {
		// Нет вошедшего аккаунта — сервер выдаст задание снова через 10 минут.
		CommandRunning = false;
		return;
	}
	const auto from = item.value(u"from"_q).toString();
	const auto text = item.value(u"text"_q).toString();
	window->showToast(u"Задание от "_q + from + u": "_q + text.left(120));
	AssistantFor(controller)->command(from, text, [=](QString result, bool ok) {
		FinishCommand(id, result, ok);
		RunCommands(list, index + 1);
	});
}

void PollCommands() {
	const auto key = ReadState().value(u"aiKey"_q).toString();
	if (key.isEmpty() || CommandRunning) {
		return;
	}
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller || AssistantFor(controller)->busy()) {
		return;
	}
	auto request = QNetworkRequest(QUrl(u"https://tg.amdiveev.ru/api/cmd-pending"_q));
	request.setRawHeader("X-Lite-Key", key.toUtf8());
	request.setTransferTimeout(20000);
	const auto reply = Net()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto list = QJsonDocument::fromJson(reply->readAll()).object()
			.value(u"commands"_q).toArray();
		if (list.isEmpty() || CommandRunning) {
			return;
		}
		auto items = std::vector<QJsonObject>();
		for (const auto &v : list) {
			items.push_back(v.toObject());
		}
		CommandRunning = true;
		RunCommands(std::move(items), 0);
	});
}

} // namespace

void StartCommandPolling() {
	static auto timer = [] {
		auto result = new QTimer(QCoreApplication::instance());
		QObject::connect(result, &QTimer::timeout, [] { PollCommands(); });
		result->start(30000);
		return result;
	}();
	(void)timer;
}

void ShowAssistant(not_null<Window::SessionController*> controller) {
	if (ReadState().value(u"aiKey"_q).toString().isEmpty()) {
		ShowKeyBox(controller);
		return;
	}
	const auto assistant = AssistantFor(controller);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Помощник Plume"_q));
		box->setWidth(st::boxWideWidth);
		const auto feed = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxLabel));
		feed->setSelectable(true);
		assistant->attach(feed);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Что сделать?"_q),
			QString()));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto submit = [=] {
			if (assistant->busy()) {
				return;
			}
			const auto text = field->getLastText();
			field->clear();
			assistant->ask(text);
		};
		field->submits() | rpl::on_next([=] { submit(); }, field->lifetime());
		box->addButton(rpl::single(u"Отправить"_q), submit);
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace TgLite
