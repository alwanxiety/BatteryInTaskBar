#pragma once
#include <string>

// Монитор AirPods: слушает Bluetooth LE-рекламу Apple и достаёт из неё заряд
// левого наушника, правого и кейса. Работает в собственном потоке, публикует
// результат в AppState::UpdateAirPods().
//
// Почему реклама, а не GATT: AirPods не отдают PKEY_BatteryLevel и не держат
// стандартный Battery Service (0x180F) для не-Apple хостов. Зато в BLE-рекламе
// Apple Continuity есть отдельное сообщение с зарядом всех трёх компонентов.
class AirPodsMonitor {
public:
    static AirPodsMonitor& Instance();

    /// Запустить фоновый поток. Повторный вызов безопасен.
    bool Start();
    /// Остановить поток. Возвращает false, если WinRT-вызов не завершился за
    /// отведённое время: тогда объект нельзя удалять и DLL нельзя выгружать.
    bool Stop();

    /// Секция отчёта: последние увиденные Apple-рекламы в hex, состояние скана
    /// и причина, по которой данных нет. Нужна для отладки парсера на живом
    /// устройстве: формат сообщений отличается между поколениями AirPods.
    void AppendDiagnostics(std::wstring& out) const;

private:
    AirPodsMonitor() = default;
    ~AirPodsMonitor();
    AirPodsMonitor(const AirPodsMonitor&) = delete;
    AirPodsMonitor& operator=(const AirPodsMonitor&) = delete;

    struct Impl;
    Impl* m_impl{ nullptr };
};
