/*
    Эта программа является примером работы захвата изображения рабочего стола
    Сжатием в jpeg кадры и дальнейшей отправкой udp пакетами на адрес приемника

    Программа использует DirectX 11 для быстрого захвата изображения
    Используется библиотека TurboJPEG для сжатия кадров
    
    Пример приемника кадров и их возпроизведения есть в другом моём репозитории:
    https://github.com/D3Hades/SimpleMJPEGPlayer
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <ws2tcpip.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <atlbase.h>
#include <windows.h>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include <turbojpeg.h>

#pragma execution_character_set("utf-8")

using namespace Microsoft::WRL;
using namespace std::chrono_literals;

// Адрес для отправки
const char* ADDRESS = "127.0.0.1";
// Номер порта для отправки
constexpr auto PORT = 57956;
// Размер пакета 7 байт заголовки и 1300 байт или меньше сам фрагмент кадра
constexpr auto PACKET_SIZE = 1307;

// Настройка качества сжатия Jpeg, чем выше тем больше размер кадров
constexpr auto JPEG_QUALITY = 90;

#define Report(x) { printf("Report: %s(%d), hr=0x%08x\n", __FILE__, __LINE__, (x)); }

bool sockInit(void) {
#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    return result == 0;
#else
    return true;
#endif
}

int createUDPClient(int port) {
    int sockfd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (sockfd < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return -1;
    }
    return sockfd;
}

void writeUINT16BE(uint16_t value, char packet[], int offset) {
    byte low_byte = (byte)(value & 0xFF);
    byte high_byte = (byte)((value >> 8) & 0xFF);
    packet[offset] = high_byte;
    packet[offset+1] = low_byte;
}

// Функция отправки кадров на указанный адрес
void sendFrame(byte* buffer, size_t buffer_size, int sock) {

    sockaddr_in addr;
    struct in_addr parseAddr;
    int s = inet_pton(AF_INET, ADDRESS, &addr.sin_addr);
    if (s <= 0) {
        std::cerr << "Not in presentation format" << std::endl;
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    socklen_t addrSize = sizeof(addr);

    static uint16_t frame_number = 0;
    int bytes_left = buffer_size;
    uint16_t packet_num = 0;
    int offset = 0;

    while (bytes_left > 0) {
        char packet[PACKET_SIZE];
        uint16_t payload_size = bytes_left > 1300 ? 1300 : bytes_left;
        bool last_packet = bytes_left < 1300;
        writeUINT16BE(payload_size, packet, 0);
        writeUINT16BE(frame_number, packet, 2);
        writeUINT16BE(packet_num, packet, 4);
        packet[6] = last_packet;

        memcpy(packet + 7, buffer + offset, payload_size);

        packet_num++;
        offset += payload_size;
        sendto(sock, packet, 1307, 0, (sockaddr*)&addr, addrSize);
        bytes_left -= payload_size;
    }
    frame_number++;
}

struct Image {
    std::vector<byte> bytes;
    int width = 0;
    int height = 0;
    int rowPitch = 0;
};

Image captureDesktop(ID3D11DeviceContext* deviceContext, ID3D11Device* device, IDXGIOutputDuplication* outputDuplication) {
    if (device == nullptr) { Report(E_FAIL); return {}; }

    // Create tex2dStaging which represents duplication image of desktop.
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;

    CComPtr<ID3D11Texture2D> tex2dStaging;
    {
        if (outputDuplication == nullptr) { Report(E_FAIL); return {}; }

        HRESULT hr = outputDuplication->AcquireNextFrame(500, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                std::cerr << "Timeout while waiting for the next frame." << std::endl;
            }
            else {
                std::cerr << "Failed to acquire next frame: " << std::hex << hr << std::endl;
            }
            return {};
        }

        ComPtr<ID3D11Texture2D> desktopImage;
        hr = desktopResource.As(&desktopImage);
        if (FAILED(hr)) {
            std::cerr << "Failed to query texture from resource: " << std::hex << hr << std::endl;
            outputDuplication->ReleaseFrame();
            return {};
        }

        DXGI_OUTDUPL_DESC duplDesc;
        outputDuplication->GetDesc(&duplDesc);

        const auto f = static_cast<int>(duplDesc.ModeDesc.Format);
        const auto goodFormat = f == DXGI_FORMAT_B8G8R8A8_UNORM
            || f == DXGI_FORMAT_B8G8R8X8_UNORM
            || f == DXGI_FORMAT_B8G8R8A8_TYPELESS
            || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            || f == DXGI_FORMAT_B8G8R8X8_TYPELESS
            || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        if (!goodFormat) { Report(E_FAIL); return {}; }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = duplDesc.ModeDesc.Width;
        desc.Height = duplDesc.ModeDesc.Height;
        desc.Format = duplDesc.ModeDesc.Format;
        desc.ArraySize = 1;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.MipLevels = 1;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.Usage = D3D11_USAGE_STAGING;
        hr = device->CreateTexture2D(&desc, nullptr, &tex2dStaging);
        if (FAILED(hr)) { Report(hr); return {}; }
        if (tex2dStaging == nullptr) { Report(E_FAIL); return {}; }

        deviceContext->CopyResource(tex2dStaging, desktopImage.Get());
    }

    // Lock tex2dStaging and copy its content from GPU to CPU memory.
    Image image;

    D3D11_TEXTURE2D_DESC desc;
    tex2dStaging->GetDesc(&desc);

    D3D11_MAPPED_SUBRESOURCE res;
    const auto hr = deviceContext->Map(
        tex2dStaging,
        D3D11CalcSubresource(0, 0, 0),
        D3D11_MAP_READ,
        0,
        &res
    );
    if (FAILED(hr)) { Report(hr); return {}; }
    image.width = static_cast<int>(desc.Width);
    image.height = static_cast<int>(desc.Height);
    image.rowPitch = res.RowPitch;
    image.bytes.resize(image.rowPitch * image.height);
    memcpy(image.bytes.data(), res.pData, image.bytes.size());
    deviceContext->Unmap(tex2dStaging, 0);
    outputDuplication->ReleaseFrame();
    return image;
}

byte* ConvertToJPEG(Image image, tjhandle handle, size_t *buffer_size) {
    byte *buff = nullptr;

    tj3Set(handle, TJPARAM_QUALITY, JPEG_QUALITY);
    tj3Set(handle, TJPARAM_SUBSAMP, TJSAMP_422);
    int res = tj3Compress8(handle, image.bytes.data(), image.width, 0, image.height, TJPF_BGRA, &buff, buffer_size);
    if (res != 0) {
        std::cerr << "Ошибка преобразования в JPEG: " << tjGetErrorStr() << std::endl;
        return nullptr;
    }
    return buff;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    if (!sockInit()) {
        std::cerr << "Failed to initialize Winsock\n";
        return -1;
    }

    int sock = createUDPClient(PORT);
    if (sock < 0) {
        return -1;
    }

    tjhandle handle = tjInitCompress();
    if (!handle) {
        std::cerr << "Ошибка инициализации TurboJPEG: " << tjGetErrorStr() << std::endl;
        return -1;
    }

    // Инициализация DirectX 11
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    HRESULT hr;

    D3D_FEATURE_LEVEL featureLevel;
    D3D11CreateDevice(
        nullptr,                   // Адаптер
        D3D_DRIVER_TYPE_HARDWARE, // Тип драйвера
        nullptr,                   // Без программного устройства
        0, // Флаги
        nullptr, 0,                // Уровни возможностей
        D3D11_SDK_VERSION,         // Версия SDK
        &d3dDevice,                // Устройство
        &featureLevel,             // Уровень возможностей
        &d3dContext                // Контекст устройства
    );
    // Получение DXGI адаптера и выхода
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice.As(&dxgiDevice);

    ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    ComPtr<IDXGIOutput> dxgiOutput;
    // Выбор рабочего стола для захвата
    dxgiAdapter->EnumOutputs(0, &dxgiOutput);

    ComPtr<IDXGIOutput1> dxgiOutput1;
    dxgiOutput.As(&dxgiOutput1);

    // Создание объекта дублирования
    ComPtr<IDXGIOutputDuplication> duplication;
    dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &duplication);

    std::cout << "Устройство дублирования создано успешно." << std::endl;
    
    // Запись в файл для проверок
    /*std::ofstream file("output.jpg", std::ios::binary);
    std::copy(buffer, buffer + buffer_size, std::ostreambuf_iterator<char>(file));
    file.close();*/

    while (true) {
        Image result = captureDesktop(d3dContext.Get(), d3dDevice.Get(), duplication.Get());

        size_t buffer_size = 0;
        byte* buffer = ConvertToJPEG(result, handle, &buffer_size);
        if (buffer == nullptr) {
            continue;
        }
        else {
            sendFrame(buffer, buffer_size, sock);
            tj3Free(buffer);
        }
        Sleep(30);
    }

    closesocket(sock);
    WSACleanup();

    std::cout << "Кадры успешно захвачены!" << std::endl;

    return 0;
}