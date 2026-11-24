# meter_codecs.hpp / .h are found through the include path; the JS / Python codec modules go next to the generated code.
gmp_add_example_all(example_custom_codec SCHEMA meter.yaml MAIN main EXPECTED expected_output.txt
        JS_FILES meter_codecs.js PY_FILES meter_codecs.py)
