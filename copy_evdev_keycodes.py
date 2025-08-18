import re

# Regular expression to match #define lines and capture components
pattern = re.compile(r'^(\s*#\s*define\s+)(\S+)(.*)$')

# Open the input file (adjust path if necessary)
with open('/usr/include/linux/input-event-codes.h', 'r') as f:
    for line in f:
        match = pattern.match(line)
        if match:
            prefix = match.group(1)
            macro = match.group(2)
            new_line = f"{prefix}NI_{macro} {macro}"
            print(new_line)
