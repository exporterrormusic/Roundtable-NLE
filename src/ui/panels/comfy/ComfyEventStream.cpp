/*
 * ComfyEventStream.cpp — see header.
 */
#include "panels/comfy/ComfyEventStream.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

namespace rt {

namespace {
constexpr int kOpContinuation = 0x0;
constexpr int kOpText   = 0x1;
constexpr int kOpBinary = 0x2;
constexpr int kOpClose  = 0x8;
constexpr int kOpPing   = 0x9;
constexpr int kOpPong   = 0xA;
constexpr int kRetryMs  = 2000;
constexpr qint64 kMaxMessageBytes = 64LL * 1024 * 1024;   // previews are small; guard garbage
} // namespace

ComfyEventStream::ComfyEventStream(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_retry(new QTimer(this))
{
    m_retry->setSingleShot(true);
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, &ComfyEventStream::connectNow);
    connect(m_socket, &QTcpSocket::connected, this, &ComfyEventStream::onConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ComfyEventStream::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ComfyEventStream::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_socket->state() == QAbstractSocket::UnconnectedState) onDisconnected();
    });
}

ComfyEventStream::~ComfyEventStream()
{
    m_wanted = false;
    m_socket->disconnect(this);
    m_socket->abort();
}

void ComfyEventStream::open(const QUrl& httpUrl, const QString& clientId)
{
    if (m_wanted && httpUrl == m_url && clientId == m_clientId) {
        if (m_state == State::Closed && !m_retry->isActive()) connectNow();
        return;
    }
    close();
    m_url = httpUrl;
    m_clientId = clientId;
    m_wanted = true;
    connectNow();
}

void ComfyEventStream::close()
{
    m_wanted = false;
    m_retry->stop();
    m_state = State::Closed;
    m_socket->abort();
    m_buffer.clear();
    m_fragment.clear();
}

void ComfyEventStream::connectNow()
{
    if (!m_wanted) return;
    m_buffer.clear();
    m_fragment.clear();
    m_state = State::Connecting;
    m_socket->abort();
    m_socket->connectToHost(m_url.host().isEmpty() ? QStringLiteral("127.0.0.1") : m_url.host(),
                            static_cast<quint16>(m_url.port(8188)));
}

void ComfyEventStream::onConnected()
{
    QByteArray key(16, Qt::Uninitialized);
    for (char& c : key) c = static_cast<char>(QRandomGenerator::global()->bounded(256));
    const QByteArray request =
        "GET /ws?clientId=" + QUrl::toPercentEncoding(m_clientId) + " HTTP/1.1\r\n"
        "Host: " + m_url.host().toUtf8() + ":" + QByteArray::number(m_url.port(8188)) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key.toBase64() + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    m_state = State::Handshake;
    m_socket->write(request);
}

void ComfyEventStream::onReadyRead()
{
    feed(m_socket->readAll());
}

void ComfyEventStream::feed(const QByteArray& bytes)
{
    m_buffer += bytes;
    if (m_state != State::Open) {   // handshake response first
        const qsizetype end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) return;
        const QByteArray statusLine = m_buffer.left(m_buffer.indexOf("\r\n"));
        m_buffer.remove(0, end + 4);
        if (!statusLine.contains(" 101")) {   // not upgraded (old ComfyUI / wrong port)
            m_state = State::Closed;
            m_socket->abort();
            if (m_wanted) m_retry->start();
            return;
        }
        m_state = State::Open;
    }
    if (m_state == State::Open) parseFrames();
}

void ComfyEventStream::parseFrames()
{
    while (m_buffer.size() >= 2) {
        const auto* p = reinterpret_cast<const uchar*>(m_buffer.constData());
        const bool fin = (p[0] & 0x80) != 0;
        const int opcode = p[0] & 0x0F;
        const bool masked = (p[1] & 0x80) != 0;
        qint64 length = p[1] & 0x7F;
        qsizetype pos = 2;
        if (length == 126) {
            if (m_buffer.size() < 4) return;
            length = qFromBigEndian<quint16>(p + 2);
            pos = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10) return;
            length = static_cast<qint64>(qFromBigEndian<quint64>(p + 2));
            pos = 10;
        }
        if (length < 0 || length > kMaxMessageBytes) {   // corrupt stream: start over
            close();
            m_wanted = true;
            m_retry->start();
            return;
        }
        uchar mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (m_buffer.size() < pos + 4) return;
            std::copy(p + pos, p + pos + 4, mask);
            pos += 4;
        }
        if (m_buffer.size() < pos + length) return;
        QByteArray payload = m_buffer.mid(pos, length);
        m_buffer.remove(0, pos + length);
        if (masked)
            for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);

        if (opcode == kOpContinuation) {
            m_fragment += payload;
            if (fin) {
                dispatch(m_fragmentOpcode, m_fragment);
                m_fragment.clear();
            }
        } else if (opcode == kOpText || opcode == kOpBinary) {
            if (fin) {
                dispatch(opcode, payload);
            } else {
                m_fragmentOpcode = opcode;
                m_fragment = payload;
            }
        } else if (opcode == kOpPing) {
            sendFrame(kOpPong, payload);
        } else if (opcode == kOpClose) {
            m_state = State::Closed;
            m_socket->disconnectFromHost();
            return;
        }
        // kOpPong: ignore
    }
}

void ComfyEventStream::dispatch(int opcode, const QByteArray& payload)
{
    if (opcode == kOpText) dispatchText(payload);
    else if (opcode == kOpBinary) dispatchBinary(payload);
}

void ComfyEventStream::dispatchText(const QByteArray& payload)
{
    const QJsonObject msg = QJsonDocument::fromJson(payload).object();
    const QString type = msg.value(QLatin1String("type")).toString();
    const QJsonObject data = msg.value(QLatin1String("data")).toObject();
    const QString promptId = data.value(QLatin1String("prompt_id")).toString();
    emit textMessage(type);
    if (type == QLatin1String("progress")) {
        const QJsonValue node = data.value(QLatin1String("node"));
        emit progress(promptId, node.isString() ? node.toString() : QString::number(node.toInt()),
                      static_cast<int>(data.value(QLatin1String("value")).toDouble()),
                      static_cast<int>(data.value(QLatin1String("max")).toDouble()));
    } else if (type == QLatin1String("executing")) {
        const QJsonValue node = data.value(QLatin1String("node"));
        emit executing(promptId, node.isNull() ? QString()
                                 : node.isString() ? node.toString() : QString::number(node.toInt()));
    } else if (type == QLatin1String("kj_preview_override")) {
        // KJNodes' preview override suppresses ComfyUI's binary previews and
        // sends its own as base64 in JSON: a JPEG (one frame / the initial
        // noise, no mime) or an animated MP4 / WebP of the whole clip.
        const QByteArray bytes = QByteArray::fromBase64(data.value(QLatin1String("image")).toString().toLatin1());
        if (bytes.isEmpty()) return;
        const QString mime = data.value(QLatin1String("mime")).toString();
        if (mime.isEmpty() || mime == QLatin1String("image/jpeg") || mime == QLatin1String("image/png")) {
            QImage decoded;
            if (decoded.loadFromData(bytes)) emit previewImage(decoded);
        } else {
            emit previewClip(bytes, mime, data.value(QLatin1String("fps")).toDouble());
        }
    }
}

void ComfyEventStream::dispatchBinary(const QByteArray& payload)
{
    if (payload.size() < 8) return;
    const auto* p = reinterpret_cast<const uchar*>(payload.constData());
    const quint32 type = qFromBigEndian<quint32>(p);
    QByteArray image;
    if (type == 1) {
        // PREVIEW_IMAGE: [format][bytes] from ComfyUI itself, but
        // VideoHelperSuite's animated latent previews send
        // [1][1][frame index][16-byte node id][JPEG]. Find the image by its
        // JPEG / PNG signature rather than trusting a fixed header size.
        qsizetype at = -1;
        for (const QByteArray& magic : {QByteArray("\xFF\xD8\xFF", 3), QByteArray("\x89PNG", 4)}) {
            const qsizetype i = payload.indexOf(magic, 8);
            if (i >= 0 && i <= 64 && (at < 0 || i < at)) at = i;
        }
        if (at < 0) return;
        image = payload.mid(at);
    } else if (type == 4) {                    // PREVIEW_IMAGE_WITH_METADATA: [len][json][bytes]
        const quint32 metaLen = qFromBigEndian<quint32>(p + 4);
        if (8 + static_cast<qsizetype>(metaLen) > payload.size()) return;
        image = payload.mid(8 + metaLen);
    } else {
        return;
    }
    QImage decoded;
    if (decoded.loadFromData(image)) emit previewImage(decoded);
}

void ComfyEventStream::sendFrame(int opcode, const QByteArray& payload)
{
    if (m_state != State::Open || payload.size() > 125) return;   // only control frames are sent
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | opcode));
    frame.append(static_cast<char>(0x80 | payload.size()));     // clients must mask
    char mask[4];
    for (char& c : mask) c = static_cast<char>(QRandomGenerator::global()->bounded(256));
    frame.append(mask, 4);
    for (qsizetype i = 0; i < payload.size(); ++i) frame.append(static_cast<char>(payload[i] ^ mask[i % 4]));
    m_socket->write(frame);
}

void ComfyEventStream::onDisconnected()
{
    const bool wasOpen = m_state != State::Closed;
    m_state = State::Closed;
    if (m_wanted && (wasOpen || !m_retry->isActive())) m_retry->start();
}

} // namespace rt
