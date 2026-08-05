#pragma once

#include <QObject>
#include <QString>

#include <atomic>

class QTcpSocket;
class QTimer;

namespace platform {

// =====wjy====
class SharedStorageAvailabilityService final : public QObject {
    Q_OBJECT

public:
    static SharedStorageAvailabilityService& instance();
    static QString hostAddress(); // wjy: 更新和桌面壁纸共享同一台文件服务器，连接测试只维护一个权威地址。
    static quint16 smbPort(); // wjy: 通过 SMB 端口判断服务器是否在线，探测过程不枚举 UNC 目录也不创建工作线程。

    bool isAvailable() const; // wjy: 返回最近一次异步探测结果，业务功能只有为 true 时才允许访问共享目录。
    bool probeInProgress() const;
    void requestProbe(); // wjy: 使用 Qt 事件循环异步连接文件服务器，主线程不等待、后台线程不增加。
    void stop(); // wjy: 程序退出时中止尚未完成的 TCP 探测，不保留网络回调。

signals:
    void availabilityChanged(bool available); // wjy: 仅连接状态真正变化时广播，供界面更新长期可用状态。
    void probeFinished(bool available); // wjy: 每次探测结束都广播，使更新、回撤和壁纸可以共享同一轮结果。

private:
    explicit SharedStorageAvailabilityService(QObject* parent = nullptr);
    void finishProbe(bool available); // wjy: 统一关闭套接字和超时计时，再发布本轮连接测试结果。

    QTcpSocket* m_socket = nullptr;
    QTimer* m_timeoutTimer = nullptr;
    std::atomic_bool m_available{false}; // wjy: 主线程更新探测结果、后台更新/回撤/壁纸任务读取该门禁时使用原子同步，避免普通 bool 跨线程数据竞争。
    bool m_probeInProgress = false;
};
// ===end====

} // namespace platform
