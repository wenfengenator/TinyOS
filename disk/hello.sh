#!/BASH.BIN

name=TinyOS

greet() {
    echo "Hello from $name scripting"
}

greet
if test "$name" = TinyOS; then
    echo "Conditionals work"
fi

for item in variables functions loops pipelines; do
    echo "  $item"
done