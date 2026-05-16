# Course DBMS

Мини-СУБД для курсовой по системному программированию.

Вариант индекса: B+-tree. Индекс подключается из соседнего проекта `FIIT_SP`.

Для разбора SQL-подобных команд используется `Boost.Spirit Qi`.
Для JSON-вывода используется рекомендованная библиотека `nlohmann/json`.
Если системный пакет не установлен, CMake автоматически загружает header-only версию через `FetchContent`.

## Сборка

```bash
cmake -S . -B build
cmake --build build
```

## Запуск

Интерактивно:

```bash
./build/course_dbms
```

Пакетно:

```bash
./build/course_dbms script.sql
```

Данные хранятся в каталоге `db_data` рядом с местом запуска программы.

## Уже реализовано

- `CREATE DATABASE`, `DROP DATABASE`, `USE`
- `CREATE TABLE`, `DROP TABLE`
- `INSERT INTO ... VALUE ...`
- `SELECT *|(...) FROM ... WHERE ...`
- `UPDATE ... SET ... WHERE ...`
- `DELETE FROM ... WHERE ...`
- типы `int`, `string`, `NULL`
- ограничения `NOT_NULL`, `INDEXED`
- дополнительное задание 10: `DEFAULT [value]` в `CREATE TABLE`
- дополнительное задание 12: агрегаты `COUNT`, `SUM`, `AVG` в `SELECT`
- уникальные индексы на `INDEXED` колонках через B+-tree
- разбор команд через библиотечный парсер `Boost.Spirit Qi`
- JSON-вывод для `SELECT` через `nlohmann/json`
- пакетный и интерактивный режимы

## Примеры допзаданий

Значения по умолчанию:

```sql
CREATE TABLE orders (
  id int INDEXED,
  customer string DEFAULT "guest",
  amount int DEFAULT 10
);

INSERT INTO orders (id, amount) VALUE (1, 25);
```

Агрегаты:

```sql
SELECT (COUNT(*) AS rows, SUM(amount) AS total, AVG(amount) AS avg_amount)
FROM orders
WHERE id >= 1;
```

## Почему Boost.Spirit вместо Flex/Bison

Flex/Bison разрешены и рекомендованы, но в текущем WSL-окружении они не установлены, а установка через `sudo`
требует пароль. Чтобы проект оставался собираемым без ручной настройки окружения, разбор команд перенесен
на `Boost.Spirit Qi`, который уже доступен вместе с Boost и не требует отдельного генератора кода.
