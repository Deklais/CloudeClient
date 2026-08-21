<div align="center">

# ☁️ CloudeClient

### Быстрый и удобный DDNet-клиент для KoG

[![Windows](https://img.shields.io/badge/Windows-supported-2496ED?style=for-the-badge&logo=windows11&logoColor=white)](https://github.com/Deklais/CloudeClient/actions)
[![DDNet](https://img.shields.io/badge/DDNet-19.8.0-F5A623?style=for-the-badge)](https://ddnet.org/)
[![Build](https://img.shields.io/github/actions/workflow/status/Deklais/CloudeClient/build.yml?branch=main&style=for-the-badge&label=BUILD)](https://github.com/Deklais/CloudeClient/actions/workflows/build.yml)
[![Fork](https://img.shields.io/badge/Fork-TClient-8B5CF6?style=for-the-badge)](https://github.com/TaterClient/TClient)

*Привычный DDNet с дополнительными настройками, улучшенным интерфейсом и полезными функциями для игры.*

</div>

---

## Возможности

| Функция | Что она делает |
|---|---|
| **Fast Input** | Уменьшает визуальную задержку управления. Доступны режимы Tater, Saiko, Cloude и Cloude+ |
| **Media Island** | Показывает трек из браузера, обложку, время и управление музыкой прямо в игре |
| **Обновлённый интерфейс** | Отдельное меню Cloude, редактор HUD и выбор режима запуска |
| **Цветные ники** | Градиентные ники игроков на основе цвета команды |
| **GIF-превью в чате** | Отображает поддерживаемые GIF-ссылки прямо в игровом чате |
| **Motion Blur** | Добавляет настраиваемый эффект размытия движения |
| **Пресеты** | Сохраняет, применяет и позволяет делиться наборами настроек |
| **Визуальные эффекты** | Дождь, дополнительные HUD-элементы и другие настройки отображения |

## 🚀 Установка

1. Откройте раздел [Actions](https://github.com/Deklais/CloudeClient/actions) или [Releases](https://github.com/Deklais/CloudeClient/releases).
2. Скачайте актуальную сборку для Windows.
3. Распакуйте **весь архив** в отдельную папку.
4. Запустите `DDNet.exe`.

> Не переносите только один EXE: клиенту нужны файлы из папки `data`.

## 🛠️ Сборка из исходников

Требуются CMake, Ninja и компилятор с поддержкой C++17.

```bash
git clone --recursive https://github.com/Deklais/CloudeClient.git
cd CloudeClient
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target DDNet
```

Подробности по зависимостям доступны в [инструкции DDNet](https://github.com/ddnet/ddnet#building-on-linux-and-macos).

## 💙 Проект

CloudeClient создан как простой клиент для комфортной игры на KoG и продолжает развиваться.

Проект является форком [TaterClient/TClient](https://github.com/TaterClient/TClient), который, в свою очередь, основан на [DDNet](https://github.com/ddnet/ddnet). Благодарность авторам и участникам обоих проектов.

<div align="center">

**CloudeClient · Сделано для игры, а не для настройки игры**

</div>
