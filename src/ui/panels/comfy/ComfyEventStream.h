/*
 * ComfyEventStream — ComfyUI's live event feed (ws://host:port/ws?clientId=…)
 * for render progress and preview images.
 *
 * A minimal receive-side WebSocket client over QTcpSocket (the build has no
 * QtWebSockets): HTTP upgrade, frame parsing (fragments, 16/64-bit lengths),
 * ping → pong, close.  Reconnects while open() is in effect, because the
 * runner restarts ComfyUI on failures.
 *
 * Messages used:
 *   text   {"type":"progress","data":{"value","max","prompt_id","node"}}
 *   text   {"type":"executing","data":{"node","prompt_id"}}   (node null = done)
 *   binary [u32 BE type][…]  type 1 = PREVIEW_IMAGE ([u32 format][image]),
 *                            type 4 = PREVIEW_IMAGE_WITH_METADATA
 *                                     ([u32 json length][json][image])
 */

#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>

class QTcpSocket;
class QTimer;

namespace rt {

class ComfyEventStream : public QObject
{
    Q_OBJECT
public:
    explicit ComfyEventStream(QObject* parent = nullptr);
    ~ComfyEventStream() override;

    /// Connect (and keep reconnecting) to `httpUrl`'s /ws as `clientId`.
    /// Calling again with the same arguments is a no-op.
    void open(const QUrl& httpUrl, const QString& clientId);
    void close();
    [[nodiscard]] bool isConnected() const noexcept { return m_state == State::Open; }

    /// Parse raw bytes as if received from the server (tests; also the
    /// socket's read path).
    void feed(const QByteArray& bytes);

signals:
    void progress(const QString& promptId, const QString& node, int value, int max);
    void executing(const QString& promptId, const QString& node);   ///< node empty = prompt finished
    void previewImage(const QImage& image);
    /// Animated preview (KJNodes "Model Preview Override": MP4 or WebP bytes,
    /// played at `fps`).  Still previews from that node come as previewImage.
    void previewClip(const QByteArray& data, const QString& mime, double fps);
    /// Every text message's "type" (diagnostics / tests).
    void textMessage(const QString& type);

private:
    enum class State { Closed, Connecting, Handshake, Open };

    void connectNow();
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void parseFrames();
    void dispatch(int opcode, const QByteArray& payload);
    void dispatchText(const QByteArray& payload);
    void dispatchBinary(const QByteArray& payload);
    void sendFrame(int opcode, const QByteArray& payload);

    QTcpSocket* m_socket{nullptr};
    QTimer*     m_retry{nullptr};
    QUrl        m_url;
    QString     m_clientId;
    bool        m_wanted{false};
    State       m_state{State::Closed};
    QByteArray  m_buffer;
    QByteArray  m_fragment;
    int         m_fragmentOpcode{0};
};

} // namespace rt
