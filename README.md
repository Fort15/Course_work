# Course DBMS

Мини-СУБД для курсовой по системному программированию.

Вариант индекса: B+-tree. Реализация дерева лежит в `include/b_plus_tree.h`.

Лексический анализ реализован через Flex, синтаксический анализ — через Bison.
Для JSON-вывода используется рекомендованная библиотека `nlohmann/json`.
Если системный пакет не установлен, CMake автоматически загружает header-only версию через `FetchContent`.

## Сборка

```bash
cmake -S . -B build
cmake --build build
```

Для сборки нужны `flex` и `bison`:

```bash
sudo apt install flex bison
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
- `REVERT [table_name] [yyyy.mm.dd-hh:mm:ss.msmsms]`
- типы `int`, `string`, `NULL`
- ограничения `NOT_NULL`, `INDEXED`
- уникальные индексы на `INDEXED` колонках через B+-tree
- лексер на Flex и парсер на Bison
- JSON-вывод для `SELECT` через `nlohmann/json`
- пакетный и интерактивный режимы

## Дополнительные задания

- 1: темпоральная персистентность `REVERT` через журнал изменений `history.log`
- 2: string interning/deduplication для строковых значений в оперативной памяти
- 7: access logs в `db_data/access.log`
- 10: `DEFAULT [value]` в `CREATE TABLE`
- 11: составные условия `AND`, `OR`, группировка через скобки
- 12: агрегаты `COUNT`, `SUM`, `AVG` в `SELECT`

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

## Парсер

Файлы грамматики находятся в `parser/`:

- `sql_lexer.l` — правила лексического анализа Flex;
- `sql_parser.y` — грамматика команд Bison.

CMake автоматически генерирует C++-исходники парсера в каталоге сборки.

## Архитектура проекта

- `src/main.cpp` — только точка входа приложения.
- `src/dbms.cpp` — движок выполнения команд, таблицы, файловое хранение, индексы и режимы запуска.
- `include/db_ast.h` — общие структуры AST, значений, условий, проекций и команд.
- `include/app.h` — публичная функция запуска приложения.
- `parser/sql_lexer.l` и `parser/sql_parser.y` — Flex/Bison слой разбора SQL-подобного языка.
- `docs/Пояснительная_записка_СУБД_B_plus_tree.docx` — пояснительная записка.
