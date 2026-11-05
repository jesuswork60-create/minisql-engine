-- A small lending library: run with  minisql examples/library.sql

CREATE TABLE authors (
  id INT PRIMARY KEY,
  name TEXT NOT NULL
);

CREATE TABLE books (
  id INT PRIMARY KEY,
  title TEXT NOT NULL,
  author_id INT,
  pages INT,
  available BOOL DEFAULT TRUE
);

CREATE INDEX books_author ON books (author_id);

INSERT INTO authors VALUES (1, 'Ursula Vale'), (2, 'Frank Marsh'), (3, 'Ines Okoro');

INSERT INTO books (id, title, author_id, pages) VALUES
  (10, 'Dune Harbour', 2, 412),
  (11, 'The Salt Road', 1, 280),
  (12, 'Winter Orchard', 1, 315),
  (13, 'Glass Rivers', 3, NULL),
  (14, 'Ash and Ember', 2, 198);

UPDATE books SET available = FALSE WHERE id = 11;

-- Which author wrote what, longest first.
SELECT b.title, a.name, b.pages
FROM books AS b
JOIN authors AS a ON b.author_id = a.id
ORDER BY b.pages DESC;

-- Authors with more than one book.
SELECT a.name, COUNT(*) AS books, AVG(b.pages) AS mean_pages
FROM authors a
JOIN books b ON b.author_id = a.id
GROUP BY a.name
HAVING COUNT(*) > 1
ORDER BY books DESC, a.name;

-- The index on author_id is used for this lookup.
EXPLAIN SELECT title FROM books WHERE author_id = 2;
