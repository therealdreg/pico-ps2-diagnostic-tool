import sys

def validate_file(filename):
    expected_size = 983077
    valid = True
    
    with open(filename, "rb") as file:
        for line_number, line in enumerate(file, start=1):
            if line.startswith(b"unsigned char array_"):
                line_size = len(line)
                if line_size != expected_size:
                    print(f"Error on line {line_number}: size {line_size} bytes, expected {expected_size} bytes")
                    print(f"Line {line_number} content (first 50 bytes): {line[:50]!r}\n")
                    valid = False
    
    return valid

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <file>")
    sys.exit(1)

file_path = sys.argv[1]

if validate_file(file_path):
    print("OK: All lines have the correct size.")
else:
    print("ERROR: Some lines have an incorrect size.")
