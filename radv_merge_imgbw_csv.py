#!/usr/bin/python3
"""Merge CSV results into one XLSX worksheet with normalized comparisons.

Usage: /usr/bin/python3 merge_imgbw_csv.py INPUT_DIRECTORY OUTPUT.xlsx
"""

import argparse
import csv
import math
from pathlib import Path

import xlsxwriter
from xlsxwriter.utility import xl_rowcol_to_cell


def load_csv_files(directory):
    """Validate the tables, then separate the shared labels from their data."""
    print(f'Finding and sorting CSV files in {directory}...', flush=True)
    paths = sorted(path for path in directory.glob('*.csv') if path.is_file())
    if not paths:
        raise ValueError(f'No CSV files found in {directory}')

    tables = []
    first_header = None
    row_count = None
    column_count = None

    for index, path in enumerate(paths, start=1):
        print(f'Loading CSV {index}/{len(paths)}: {path.name}...', flush=True)
        with path.open(newline='', encoding='utf-8-sig') as stream:
            rows = list(csv.reader(stream))

        print(f'Validating dimensions and header of {path.name}...', flush=True)
        if not rows or len(rows[0]) < 2:
            raise ValueError(f'{path}: expected a header with at least two columns')

        if first_header is None:
            first_header = rows[0]
            row_count = len(rows)
            column_count = len(first_header)

        if len(rows) != row_count:
            raise ValueError(f'{path}: expected {row_count} rows, got {len(rows)}')
        for number, row in enumerate(rows, start=1):
            if len(row) != column_count:
                raise ValueError(
                    f'{path}, row {number}: expected {column_count} columns, '
                    f'got {len(row)}')
        if rows[0] != first_header:
            raise ValueError(f'{path}: header differs from {paths[0]}')

        tables.append((path, rows))

    print('Separating the shared labels from the CSV tables...', flush=True)
    labels = [row[0] for row in tables[0][1]]
    tables = [(path, [row[1:] for row in rows]) for path, rows in tables]
    return labels, tables


def write_value(sheet, row, column, text, number_format):
    """Write numbers as numbers and preserve other CSV contents as text."""
    try:
        number = float(text)
    except ValueError:
        number = None

    if number is not None and math.isfinite(number):
        sheet.write_number(row, column, number, number_format)
    else:
        sheet.write_string(row, column, text)


def write_workbook(output, labels, tables):
    print('Calculating table positions and checking XLSX size limits...', flush=True)
    row_count = len(labels)
    table_width = len(tables[0][1][0])
    table_count = len(tables)
    # Column A contains labels. Each table occupies its width plus a separator.
    starts = [1 + index * (table_width + 1) for index in range(2 * table_count)]
    if row_count + 1 > 1048576 or starts[-1] + table_width > 16384:
        raise ValueError('The combined tables exceed the XLSX row or column limit')

    print('Creating the worksheet and setting up formatting and frozen panes...', flush=True)
    with xlsxwriter.Workbook(str(output), {'use_future_functions': True}) as book:
        sheet = book.add_worksheet('imgbw')
        bold = book.add_format({'bold': True})
        percent = book.add_format({'num_format': '0.00%'})
        # Display fewer decimal places for larger bandwidth values.
        number_format = book.add_format({
            'num_format': '[<10000]0.00;[<100000]0.0;0',
        })

        sheet.freeze_panes(2, 1)
        sheet.set_column_pixels(0, 0, 580)
        sheet.set_row(0, None, bold)
        sheet.set_row(1, None, bold)

        print('Writing the shared labels to column A...', flush=True)
        for row, label in enumerate(labels, start=1):
            sheet.write_string(row, 0, label)

        # First write the original values from every CSV.
        for index, (path, rows) in enumerate(tables):
            print(f'Writing original table {index + 1}/{table_count}: {path.stem}...',
                  flush=True)
            start = starts[index]
            sheet.write_string(0, start, path.stem, bold)
            for column, header in enumerate(rows[0], start=start):
                sheet.write_string(1, column, header, bold)
            for row, values in enumerate(rows[1:], start=2):
                for column, value in enumerate(values, start=start):
                    write_value(sheet, row, column, value, number_format)

        # Then append a matching set of tables containing normalized formulas.
        for index, (path, rows) in enumerate(tables):
            print(f'Writing percentage formulas for table {index + 1}/{table_count}: '
                  f'{path.stem}...', flush=True)
            start = starts[table_count + index]
            sheet.write_string(0, start, path.stem, bold)
            for column, header in enumerate(rows[0], start=start):
                sheet.write_string(1, column, header, bold)

            for row in range(2, row_count + 1):
                for column in range(table_width):
                    references = [xl_rowcol_to_cell(row, original_start + column)
                                  for original_start in starts[:table_count]]
                    value = references[index]
                    maximum = ','.join(references)
                    formula = (f'=LET(x,IFERROR({value}/MAX({maximum}),"missing"),'
                               'IF(ISNUMBER(x),x,"n/a"))')
                    sheet.write_formula(row, start + column, formula, percent)

            if row_count > 1:
                print(f'Applying the color scale to {path.stem}...', flush=True)
                sheet.conditional_format(2, start, row_count, start + table_width - 1, {
                    'type': '3_color_scale',
                    'min_type': 'num', 'min_value': 0.5, 'min_color': '#FF0000',
                    'mid_type': 'num', 'mid_value': 1.0, 'mid_color': '#FFFFFF',
                    'max_type': 'num', 'max_value': 1.1, 'max_color': '#FFFFFF',
                })

        print(f'Saving the XLSX file to {output}...', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input_directory', type=Path)
    parser.add_argument('output_file', type=Path)
    args = parser.parse_args()

    print('This may take a minute to finish.', flush=True)
    print('Checking the input directory...', flush=True)
    if not args.input_directory.is_dir():
        parser.error(f'Not a directory: {args.input_directory}')

    try:
        labels, tables = load_csv_files(args.input_directory)
        print('Checking that the output path does not overwrite an input CSV...', flush=True)
        if any(args.output_file.resolve() == path.resolve() for path, _ in tables):
            raise ValueError('The output file must not overwrite an input CSV')
        write_workbook(args.output_file, labels, tables)
    except (OSError, ValueError, csv.Error, xlsxwriter.exceptions.XlsxWriterException) as error:
        parser.exit(1, f'Error: {error}\n')

    print(f'Saved {args.output_file}', flush=True)


if __name__ == '__main__':
    main()
