/*
Plume (TG Lite): ИИ-помощник Takt на компьютере — отдельное окно-переписка.
Кроме чатов и папок управляет ботом Такт (@takt52_bot, отвечает людям за владельца через «Автоматизацию чатов»)
через API его сайта takt.amdiveev.ru: ключ Plume получает сам — пишет боту «plume» и читает ответ.
Сам Plume за владельца на входящие не отвечает: это делает только Такт (официальный Bot API, без риска для аккаунта).
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
#include "base/unixtime.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mainwindow.h"
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
#include <QtCore/QRegularExpression>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace TgLite {
namespace {

constexpr auto kEndpoint = "https://tg.amdiveev.ru/api/ai";
constexpr auto kStepsMax = 15;
constexpr auto kBroadcastOnce = 25;
constexpr auto kBroadcastDaily = 60;
constexpr auto kTaktBase = "https://takt.amdiveev.ru";
constexpr auto kTaktBotId = 8909004776ULL; // @takt52_bot
constexpr auto kFeedMax = 80;

QString StatePath() {
	return cWorkingDir() + u"tglite.json"_q;
}

QJsonObject ReadState() {
	QFile f(StatePath());
	return f.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(f.readAll()).object()
		: QJsonObject();
}

// Ключ ИИ-помощника встроен при сборке (-D TGLITE_AI_KEY); свой, введённый вручную, — главнее.
#ifdef TGLITE_AI_KEY
constexpr auto kEmbeddedAiKey = TGLITE_AI_KEY;
#else // TGLITE_AI_KEY
constexpr auto kEmbeddedAiKey = "";
#endif // TGLITE_AI_KEY

QString AiKey() {
	const auto own = ReadState().value(u"aiKey"_q).toString();
	return own.isEmpty() ? QString::fromLatin1(kEmbeddedAiKey) : own;
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
		Tool(u"takt_status"_q,
			u"Состояние Такта (бот, который отвечает людям за пользователя): включён ли, темп, сухой прогон, «меня нет до», "
			u"кто ждёт ответа пользователя, последние записи журнала (кому и что ответил или ответил бы) и чаты Такта. "
			u"Статусы записей: draft — черновик ждёт решения, dry — сухой прогон (ответил бы, но не отправил), sent — отправлено, "
			u"skipped — промолчал, discarded — отменено, error — ошибка. «ждёт вас» — Такт пообещал, что пользователь ответит сам."_q,
			{}),
		Tool(u"takt_do"_q,
			u"Передать Такту фразу владельца, как если бы он написал её боту. Примеры: «стоп», «старт», «меня не будет до 20», "
			u"«я вернулся», «темп аллегро», «сухой прогон выкл», «Оле отвечай сам», «маме не отвечай», "
			u"«правило: цену не называй, говори „обсудим на созвоне“», «запомни: работаю с 10 до 19», "
			u"«правила», «что ты знаешь обо мне», «научись». Возвращает ответ Такта."_q,
			{ { u"text"_q, Prop(u"string"_q, u"фраза для Такта"_q) } },
			{ u"text"_q }),
		Tool(u"takt_entry"_q,
			u"Действие с записью журнала Такта (id из takt_status): send — отправить черновик или ответ сухого прогона как есть, "
			u"edit — отправить вместо него свой текст, discard — не отвечать, ok — ответ верный, "
			u"correct — «отвечать иначе» (text — как надо, станет правилом), confirm — «подтвердить» человеку, который ждёт. "
			u"send и edit пользователь подтверждает."_q,
			{ { u"id"_q, Prop(u"integer"_q, u"id записи"_q) },
			  { u"op"_q, Prop(u"string"_q, u"send | edit | discard | ok | correct | confirm"_q) },
			  { u"text"_q, Prop(u"string"_q, u"текст для edit / correct"_q) } },
			{ u"id"_q, u"op"_q }),
		Tool(u"takt_chat_mode"_q,
			u"Как Такту вести себя с человеком: allegro — отвечать самому (нейросеть), adagio — готовой фразой, "
			u"pause — молчать в этом чате, never — никогда не отвечать, пустая строка — как обычно. id — из чатов в takt_status."_q,
			{ { u"chat_id"_q, Prop(u"integer"_q, u"id чата Такта"_q) },
			  { u"mode"_q, Prop(u"string"_q, u"allegro | adagio | pause | never | пусто"_q) } },
			{ u"chat_id"_q, u"mode"_q }),
	};
}

QString SystemPrompt() {
	return u"Ты — Takt, помощник внутри Plume, личного Telegram-клиента Александра. Сейчас "_q
		+ QDateTime::currentDateTime().toString(u"dd.MM.yyyy HH:mm"_q) + u". "
		u"Отвечай по-русски, коротко и просто, обращайся на «вы». Делай то, что просят, с помощью инструментов: "
		u"находи чаты через list_chats, папки — list_folders, создавай и меняй папки, читай сообщения, отправляй. "
		u"Ещё ты управляешь ботом Такт: он сам отвечает людям за Александра, пока тот занят (через «Автоматизацию чатов» Telegram). "
		u"Что Такт ответил, кто ждёт, черновики — takt_status; команды Такту (пауза, «меня нет до», правила, факты, темп) — takt_do; "
		u"отправить или поправить черновик — takt_entry; режим для человека — takt_chat_mode. "
		u"Если просят ответить человеку, по чьему сообщению у Такта есть черновик или ответ сухого прогона, — используй takt_entry, "
		u"иначе send_message. Показывая черновики, пиши кому, что спросили и что Такт предлагает ответить. "
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
	enum class Kind {
		Me,
		Bot,
		Step,
		Info,
	};
	struct Line {
		Kind kind = Kind::Info;
		QString text;
	};

	explicit Assistant(not_null<Window::SessionController*> controller)
	: _controller(controller)
	, _session(&controller->session()) {
		// Переписка сохраняется между запусками (только то, что видно в окне; контекст модели — нет).
		for (const auto &v : ReadState().value(u"taktFeed"_q).toArray()) {
			const auto o = v.toObject();
			_lines.push_back({ Kind(o.value(u"k"_q).toInt()), o.value(u"t"_q).toString() });
		}
	}

	~Assistant() {
		delete _window.data();
	}

	void showWindow();

	[[nodiscard]] const std::vector<Line> &lines() const {
		return _lines;
	}

	void reset() {
		if (_busy) {
			return;
		}
		_messages = QJsonArray();
		_lines.clear();
		save();
		render();
	}

	// Задание из другого проекта (очередь /api/cmd): тот же цикл, итог — в done.
	void command(const QString &from, const QString &text, Fn<void(QString, bool)> done) {
		_onDone = std::move(done);
		_lastText = QString();
		say(Kind::Me, u"Задание от "_q + from + u": "_q + text.trimmed());
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
		say(Kind::Me, text.trimmed());
		trimContext();
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
	// Строка хода работы («· смотрю чаты») или служебная — серым, без пузыря.
	void add(const QString &line) {
		say(line.startsWith(u"· "_q) ? Kind::Step : Kind::Info,
			line.startsWith(u"· "_q) ? line.mid(2) : line);
	}

	void say(Kind kind, const QString &text) {
		_lines.push_back({ kind, text });
		while (_lines.size() > kFeedMax) {
			_lines.erase(_lines.begin());
		}
		save();
		render();
	}

	void save() {
		auto list = QJsonArray();
		for (const auto &line : _lines) {
			if (line.kind == Kind::Me || line.kind == Kind::Bot) {
				list.append(QJsonObject{
					{ u"k"_q, int(line.kind) },
					{ u"t"_q, line.text },
				});
			}
		}
		SetState(u"taktFeed"_q, list);
	}

	void render();

	// Контекст модели не растёт бесконечно: режем по началу реплики пользователя (не посреди tool_use / tool_result).
	void trimContext() {
		const auto isUserText = [](const QJsonValue &v) {
			const auto o = v.toObject();
			return o.value(u"role"_q).toString() == u"user"_q
				&& o.value(u"content"_q).isString();
		};
		if (_messages.size() <= 40) {
			return;
		}
		while (_messages.size() > 24 || (!_messages.isEmpty() && !isUserText(_messages.first()))) {
			_messages.removeFirst();
		}
	}

	void step() {
		if (++_steps > kStepsMax) {
			add(u"Остановился: слишком много шагов."_q);
			finish(u"остановился: слишком много шагов"_q, false);
			return;
		}
		_busy = true;
		render();
		const auto key = AiKey();
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
					say(Kind::Bot, text);
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
		if (_window && _window->isVisible()) {
			// Окно Takt открыто — спрашиваем в нём, а не в главном окне за ним.
			const auto box = new QMessageBox(
				QMessageBox::Question,
				u"Takt"_q,
				text,
				QMessageBox::NoButton,
				_window);
			box->setAttribute(Qt::WA_DeleteOnClose);
			const auto ok = box->addButton(u"Выполнить"_q, QMessageBox::AcceptRole);
			box->addButton(u"Отмена"_q, QMessageBox::RejectRole);
			QObject::connect(box, &QMessageBox::finished, [=] {
				if (box->clickedButton() == ok) {
					yes();
				} else {
					no();
				}
			});
			box->open();
			return;
		}
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
		} else if (name == u"takt_status"_q) {
			add(u"· смотрю, что делал Такт"_q);
			takt(u"/api/state"_q, std::nullopt, [=](QJsonObject data, QString error) {
				if (!error.isEmpty()) {
					done(error, true);
					return;
				}
				done(describeTakt(data), false);
			});
		} else if (name == u"takt_do"_q) {
			const auto text = input.value(u"text"_q).toString().trimmed();
			if (text.isEmpty()) {
				done(u"пустая фраза"_q, true);
				return;
			}
			add(u"· Такту: «"_q + text + u"»"_q);
			takt(u"/api/act"_q, QJsonObject{
				{ u"action"_q, u"command"_q },
				{ u"text"_q, text },
			}, [=](QJsonObject data, QString error) {
				done(error.isEmpty() ? replyText(data) : error, !error.isEmpty());
			});
		} else if (name == u"takt_entry"_q) {
			taktEntry(input, done);
		} else if (name == u"takt_chat_mode"_q) {
			const auto mode = input.value(u"mode"_q).toString().trimmed();
			const auto chatId = input.value(u"chat_id"_q).toVariant().toLongLong();
			add(u"· меняю режим Такта для чата"_q);
			takt(u"/api/act"_q, QJsonObject{
				{ u"action"_q, u"chat_mode"_q },
				{ u"id"_q, chatId },
				{ u"mode"_q, mode },
			}, [=](QJsonObject data, QString error) {
				done(error.isEmpty() ? u"готово"_q : error, !error.isEmpty());
			});
		} else {
			done(u"неизвестный инструмент"_q, true);
		}
	}

	// ---------- Такт ----------

	static QString replyText(const QJsonObject &data) {
		auto lines = QStringList();
		for (const auto &v : data.value(u"reply"_q).toArray()) {
			lines.push_back(v.toString());
		}
		return lines.isEmpty() ? u"готово"_q : lines.join(u"\n"_q);
	}

	static QString When(const QJsonValue &value) {
		const auto at = value.toVariant().toLongLong();
		return at > 0
			? QDateTime::fromSecsSinceEpoch(at).toString(u"dd.MM HH:mm"_q)
			: QString();
	}

	QString describeTakt(const QJsonObject &data) {
		const auto settings = data.value(u"settings"_q).toObject();
		const auto get = [&](const QString &key) {
			return settings.value(key).toVariant().toString();
		};
		const auto tempo = get(u"tempo"_q);
		auto lines = QStringList{
			u"Подключён к аккаунту: "_q + (data.value(u"connected"_q).toBool() ? u"да"_q : u"нет"_q)
				+ (data.value(u"can_reply"_q).toBool() ? QString() : u" (нет права отвечать)"_q),
			u"Остановлен (стоп): "_q + (get(u"stopped"_q) == u"1"_q ? u"да"_q : u"нет"_q),
			u"Темп: "_q + (tempo == u"pause"_q
				? u"пауза (молчит)"_q
				: tempo == u"adagio"_q
				? u"адажио (готовая фраза)"_q
				: u"аллегро (отвечает сам)"_q),
			u"Сухой прогон (никому не пишет, только показывает): "_q
				+ (get(u"dry_run"_q) == u"0"_q ? u"выкл"_q : u"вкл"_q),
		};
		const auto away = settings.value(u"away_until"_q).toVariant().toLongLong();
		if (away > QDateTime::currentSecsSinceEpoch()) {
			lines.push_back(u"Владельца нет до: "_q + When(away));
		}
		lines.push_back(u"Ждут ответа владельца: "_q + QString::number(data.value(u"waiting"_q).toInt())
			+ u", черновиков: "_q + QString::number(data.value(u"drafts"_q).toInt())
			+ u", ответов на проверку: "_q + QString::number(data.value(u"to_review"_q).toInt()));
		lines.push_back(QString());
		lines.push_back(u"Журнал (id | чат | статус | когда | ждёт владельца | вопрос → ответ | причина):"_q);
		auto count = 0;
		for (const auto &v : data.value(u"journal"_q).toArray()) {
			const auto e = v.toObject();
			const auto status = e.value(u"status"_q).toString();
			if (status == u"skipped"_q && count >= 10) {
				continue;
			}
			_taktEntries[e.value(u"id"_q).toVariant().toLongLong()] = {
				e.value(u"name"_q).toString(),
				e.value(u"text"_q).toString(),
			};
			lines.push_back(e.value(u"id"_q).toVariant().toString() + u" | "_q
				+ e.value(u"name"_q).toString() + u" | "_q
				+ status + u" | "_q
				+ When(e.value(u"created"_q)) + u" | "_q
				+ (e.value(u"needs_owner"_q).toInt() && !e.value(u"resolved"_q).toInt() ? u"да"_q : u"нет"_q) + u" | "_q
				+ e.value(u"question"_q).toString().left(300) + u" → "_q
				+ e.value(u"text"_q).toString().left(300) + u" | "_q
				+ e.value(u"reason"_q).toString());
			if (++count >= 30) {
				break;
			}
		}
		lines.push_back(QString());
		lines.push_back(u"Чаты Такта (id | имя | режим | последнее):"_q);
		count = 0;
		for (const auto &v : data.value(u"chats"_q).toArray()) {
			const auto c = v.toObject();
			const auto last = c.value(u"last"_q).toObject();
			lines.push_back(c.value(u"id"_q).toVariant().toString() + u" | "_q
				+ c.value(u"name"_q).toString() + u" | "_q
				+ (c.value(u"never"_q).toInt()
					? u"никогда не отвечать"_q
					: c.value(u"mode"_q).toString().isEmpty()
					? u"как обычно"_q
					: c.value(u"mode"_q).toString()) + u" | "_q
				+ (last.value(u"who"_q).toString() == u"in"_q ? u"от человека: "_q : u"от нас: "_q)
				+ last.value(u"text"_q).toString().left(150));
			if (++count >= 25) {
				break;
			}
		}
		return lines.join('\n');
	}

	void taktEntry(const QJsonObject &input, Fn<void(QString, bool)> done) {
		const auto id = input.value(u"id"_q).toVariant().toLongLong();
		const auto op = input.value(u"op"_q).toString();
		const auto text = input.value(u"text"_q).toString().trimmed();
		const auto act = [=] {
			add(u"· Такт: "_q + (op == u"send"_q || op == u"edit"_q
				? u"отправляю ответ"_q
				: op == u"discard"_q
				? u"не отвечаем"_q
				: op == u"correct"_q
				? u"запоминаю, как надо"_q
				: u"готово"_q));
			takt(u"/api/act"_q, QJsonObject{
				{ u"action"_q, u"entry"_q },
				{ u"id"_q, id },
				{ u"op"_q, op },
				{ u"text"_q, text },
			}, [=](QJsonObject data, QString error) {
				done(error.isEmpty() ? replyText(data) : error, !error.isEmpty());
			});
		};
		if (op != u"send"_q && op != u"edit"_q) {
			act();
			return;
		}
		const auto i = _taktEntries.find(id);
		const auto who = (i != end(_taktEntries)) ? i->second.first : QString();
		const auto what = (op == u"edit"_q) ? text : (i != end(_taktEntries)) ? i->second.second : QString();
		confirm(
			u"Такт отправит от вашего имени"_q
				+ (who.isEmpty() ? QString() : u" — «"_q + who + u"»"_q)
				+ u":\n\n"_q + (what.isEmpty() ? u"(текст черновика #"_q + QString::number(id) + u")"_q : what),
			act,
			[=] { done(u"пользователь отменил"_q, false); });
	}

	// Запрос к сайту Такта. Нет ключа — сначала получить его (написать боту «plume»).
	void takt(
			const QString &path,
			std::optional<QJsonObject> body,
			Fn<void(QJsonObject, QString)> done,
			bool retried = false) {
		ensureTaktKey([=, weak = base::make_weak(this)](QString error) {
			if (!weak) {
				return;
			} else if (!error.isEmpty()) {
				done({}, error);
				return;
			}
			auto request = QNetworkRequest(QUrl(QString(kTaktBase) + path));
			request.setRawHeader("X-Takt-Key", ReadState().value(u"taktKey"_q).toString().toUtf8());
			request.setRawHeader("X-Takt", "1");
			request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
			request.setTransferTimeout(60000);
			const auto reply = body
				? Net()->post(request, QJsonDocument(*body).toJson(QJsonDocument::Compact))
				: Net()->get(request);
			QObject::connect(reply, &QNetworkReply::finished, [=] {
				reply->deleteLater();
				if (!weak) {
					return;
				}
				const auto status = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				const auto data = QJsonDocument::fromJson(reply->readAll()).object();
				if (status == 401 && !retried) {
					SetState(u"taktKey"_q, QString());
					takt(path, body, done, true);
				} else if (status == 401) {
					done({}, u"Такт не принял ключ"_q);
				} else if (!data.value(u"error"_q).toString().isEmpty()) {
					done({}, data.value(u"error"_q).toString());
				} else if (reply->error() != QNetworkReply::NoError || data.isEmpty()) {
					done({}, u"нет связи с Тактом: "_q + reply->errorString());
				} else {
					done(data, QString());
				}
			});
		});
	}

	void ensureTaktKey(Fn<void(QString)> done) {
		if (!ReadState().value(u"taktKey"_q).toString().isEmpty()) {
			done(QString());
			return;
		}
		const auto bot = _session->data().userLoaded(UserId(kTaktBotId));
		if (!bot) {
			done(u"не нашёл бота Такт (@takt52_bot) — откройте его чат в Plume один раз и повторите"_q);
			return;
		}
		const auto history = _session->data().history(bot);
		add(u"· подключаю Такт: пишу боту «plume»"_q);
		const auto since = base::unixtime::now() - 5;
		send(history, u"plume"_q);
		waitTaktKey(history, since, 0, std::move(done));
	}

	void waitTaktKey(
			not_null<History*> history,
			TimeId since,
			int attempt,
			Fn<void(QString)> done) {
		if (attempt >= 12) {
			done(u"Такт не прислал ключ — проверьте, что бот @takt52_bot работает"_q);
			return;
		}
		const auto session = _session;
		base::call_delayed(2000, session, [=, weak = base::make_weak(this)] {
			if (!weak) {
				return;
			}
			session->api().request(MTPmessages_GetHistory(
				history->peer->input(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(5),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &result) {
				if (!weak) {
					return;
				}
				static const auto re = QRegularExpression(u"Ключ для Plume:\\s*([\\w-]{20,})"_q);
				auto key = QString();
				result.match([](const MTPDmessages_messagesNotModified &) {
				}, [&](const auto &data) {
					for (const auto &message : data.vmessages().v) {
						message.match([&](const MTPDmessage &m) {
							if (key.isEmpty() && !m.is_out() && m.vdate().v >= since) {
								const auto match = re.match(qs(m.vmessage()));
								if (match.hasMatch()) {
									key = match.captured(1);
								}
							}
						}, [](const auto &) {});
					}
				});
				if (key.isEmpty()) {
					waitTaktKey(history, since, attempt + 1, done);
					return;
				}
				SetState(u"taktKey"_q, key);
				add(u"· Такт подключён"_q);
				done(QString());
			}).fail([=](const MTP::Error &) {
				if (weak) {
					waitTaktKey(history, since, attempt + 1, done);
				}
			}).send();
		});
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
		render();
		if (const auto done = base::take(_onDone)) {
			done(result, ok);
		}
	}

	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;
	QPointer<QWidget> _window;
	Fn<void()> _changed;
	std::map<qint64, std::pair<QString, QString>> _taktEntries;
	QJsonArray _messages;
	QJsonArray _results;
	std::vector<Line> _lines;
	QString _lastText;
	Fn<void(QString, bool)> _onDone;
	int _steps = 0;
	bool _busy = false;

};

// Enter — отправить, Shift+Enter — новая строка.
class EnterFilter final : public QObject {
public:
	EnterFilter(QObject *parent, Fn<void()> submit)
	: QObject(parent)
	, _submit(std::move(submit)) {
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event);
			if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
				&& !(key->modifiers() & Qt::ShiftModifier)) {
				_submit();
				return true;
			}
		}
		return QObject::eventFilter(watched, event);
	}

private:
	Fn<void()> _submit;

};

QString BubbleHtml(const QString &text) {
	auto result = text.toHtmlEscaped();
	static const auto bold = QRegularExpression(u"\\*\\*([^*\\n]+)\\*\\*"_q);
	result.replace(bold, u"<b>\\1</b>"_q);
	static const auto link = QRegularExpression(u"(https?://[^\\s<]+[^\\s<.,;:!?)»])"_q);
	result.replace(link, u"<a href=\"\\1\">\\1</a>"_q);
	return result.replace('\n', u"<br>"_q);
}

void Assistant::render() {
	if (_changed) {
		_changed();
	}
}

void Assistant::showWindow() {
	if (_window) {
		_window->show();
		_window->raise();
		_window->activateWindow();
		return;
	}
	const auto window = new QWidget(nullptr, Qt::Window);
	_window = window;
	window->setWindowTitle(u"Takt — помощник Plume"_q);
	window->setWindowIcon(_controller->widget()->windowIcon());
	window->resize(460, 680);
	window->setMinimumSize(360, 440);
	window->setStyleSheet(u""
		"QWidget { background: #ffffff; color: #1b1f24; font-size: 14px; }"
		"QTextBrowser { border: none; background: #f4f6f9; padding: 6px; }"
		"QPlainTextEdit { border: 1px solid #d5dbe3; border-radius: 10px; padding: 6px 8px; background: #ffffff; }"
		"QPlainTextEdit:focus { border-color: #3a7bd5; }"
		"QPushButton { border: 1px solid #d5dbe3; border-radius: 14px; padding: 5px 12px; background: #ffffff; color: #2a5ea8; }"
		"QPushButton:hover { background: #eef3fb; }"
		"QPushButton#send { background: #3a7bd5; color: #ffffff; border: none; border-radius: 10px; padding: 8px 16px; font-weight: 600; }"
		"QPushButton#send:hover { background: #2f6bc0; }"
		"QPushButton#reset { border: none; color: #7a8592; padding: 4px 6px; }"
		"QLabel#title { font-size: 18px; font-weight: 700; }"
		"QLabel#subtitle { color: #7a8592; font-size: 12px; }"_q);

	const auto layout = new QVBoxLayout(window);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	const auto header = new QWidget(window);
	const auto headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(16, 10, 10, 10);
	const auto titles = new QVBoxLayout();
	titles->setSpacing(0);
	const auto title = new QLabel(u"Takt"_q, header);
	title->setObjectName(u"title"_q);
	const auto subtitle = new QLabel(u"отвечает людям за вас, ведёт чаты и папки"_q, header);
	subtitle->setObjectName(u"subtitle"_q);
	titles->addWidget(title);
	titles->addWidget(subtitle);
	headerLayout->addLayout(titles, 1);
	const auto resetButton = new QPushButton(u"Новый разговор"_q, header);
	resetButton->setObjectName(u"reset"_q);
	resetButton->setCursor(Qt::PointingHandCursor);
	headerLayout->addWidget(resetButton);
	layout->addWidget(header);

	const auto feed = new QTextBrowser(window);
	feed->setOpenExternalLinks(true);
	feed->setFrameShape(QFrame::NoFrame);
	layout->addWidget(feed, 1);

	const auto bottom = new QWidget(window);
	const auto bottomLayout = new QVBoxLayout(bottom);
	bottomLayout->setContentsMargins(12, 8, 12, 12);
	bottomLayout->setSpacing(8);
	const auto quick = new QHBoxLayout();
	quick->setSpacing(6);
	bottomLayout->addLayout(quick);
	const auto inputRow = new QHBoxLayout();
	inputRow->setSpacing(8);
	const auto input = new QPlainTextEdit(bottom);
	input->setPlaceholderText(u"Напишите Takt… (Enter — отправить, Shift+Enter — новая строка)"_q);
	input->setFixedHeight(64);
	const auto sendButton = new QPushButton(u"Отправить"_q, bottom);
	sendButton->setObjectName(u"send"_q);
	sendButton->setCursor(Qt::PointingHandCursor);
	inputRow->addWidget(input, 1);
	inputRow->addWidget(sendButton, 0, Qt::AlignBottom);
	bottomLayout->addLayout(inputRow);
	layout->addWidget(bottom);

	const auto weak = base::make_weak(this);
	const auto askNow = [=](const QString &text) {
		if (weak && !_busy) {
			this->ask(text);
		}
	};
	const auto quickButton = [&](const QString &label, const QString &text) {
		const auto button = new QPushButton(label, bottom);
		button->setCursor(Qt::PointingHandCursor);
		QObject::connect(button, &QPushButton::clicked, [=] { askNow(text); });
		quick->addWidget(button);
	};
	quickButton(u"Что нового"_q, u"Что нового у Такта: кому он ответил и кто ждёт моего ответа?"_q);
	quickButton(u"Черновики"_q, u"Покажи черновики Такта, которые ждут моего решения"_q);
	quickButton(u"Пауза"_q, u"Такт, стоп — никому не отвечай, пока не скажу"_q);
	quickButton(u"Продолжить"_q, u"Такт, старт — продолжай отвечать"_q);
	quick->addStretch(1);

	const auto submit = [=] {
		if (!weak || _busy) {
			return;
		}
		const auto text = input->toPlainText().trimmed();
		if (text.isEmpty()) {
			return;
		}
		input->clear();
		askNow(text);
	};
	input->installEventFilter(new EnterFilter(input, submit));
	QObject::connect(sendButton, &QPushButton::clicked, submit);
	QObject::connect(resetButton, &QPushButton::clicked, [=] {
		if (weak) {
			reset();
		}
	});

	_changed = [=] {
		auto html = QString(u"<html><body style=\"margin:4px\">"_q);
		const auto bubble = [&](bool mine, const QString &text) {
			html += mine
				? u"<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin:6px 0\"><tr><td width=\"18%\"></td>"
					"<td align=\"right\"><table cellspacing=\"0\" cellpadding=\"10\"><tr><td bgcolor=\"#3a7bd5\" style=\"color:#ffffff\">"_q
				: u"<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin:6px 0\"><tr>"
					"<td align=\"left\"><table cellspacing=\"0\" cellpadding=\"10\"><tr><td bgcolor=\"#ffffff\" style=\"color:#1b1f24\">"
					"<span style=\"color:#2a5ea8;font-weight:600;font-size:12px\">Takt</span><br>"_q;
			html += BubbleHtml(text);
			html += mine
				? u"</td></tr></table></td></tr></table>"_q
				: u"</td></tr></table></td><td width=\"18%\"></td></tr></table>"_q;
		};
		if (_lines.empty()) {
			bubble(false, u"Здравствуйте! Я Takt. Пишите обычными словами, например:\n"
				"• «что нового?» — кому Такт ответил и кто ждёт вас\n"
				"• «отправь Диме черновик» или «ответь Оле: буду в 18»\n"
				"• «меня нет до 20», «пауза», «Оле отвечай сам»\n"
				"• «правило: цену не называй»\n"
				"• «создай папку Штаб с чатами со словом 195»"_q);
		}
		for (const auto &line : _lines) {
			switch (line.kind) {
			case Kind::Me: bubble(true, line.text); break;
			case Kind::Bot: bubble(false, line.text); break;
			case Kind::Step:
				html += u"<p style=\"color:#8a94a0;font-size:12px;margin:2px 10px\">· "_q
					+ line.text.toHtmlEscaped() + u"</p>"_q;
				break;
			case Kind::Info:
				html += u"<p align=\"center\" style=\"color:#b04a3a;font-size:12px;margin:6px 10px\">"_q
					+ line.text.toHtmlEscaped() + u"</p>"_q;
				break;
			}
		}
		if (_busy) {
			html += u"<p style=\"color:#8a94a0;font-size:12px;margin:6px 10px\"><i>Takt печатает…</i></p>"_q;
		}
		html += u"</body></html>"_q;
		feed->setHtml(html);
		feed->verticalScrollBar()->setValue(feed->verticalScrollBar()->maximum());
		sendButton->setEnabled(!_busy);
	};
	render();
	window->show();
	input->setFocus();
}

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
		box->setTitle(rpl::single(u"Takt — помощник Plume"_q));
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
	request.setRawHeader("X-Lite-Key", AiKey().toUtf8());
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
	const auto key = AiKey();
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
	if (AiKey().isEmpty()) {
		ShowKeyBox(controller);
		return;
	}
	AssistantFor(controller)->showWindow();
}

} // namespace TgLite
