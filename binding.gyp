{
  "targets": [
    {
      "target_name": "xla_js",
      "sources": ["src/native/addon.cc"],
      "include_dirs": ["xla"],
      "cflags_cc": ["-std=c++20"],
      "xcode_settings": {
        "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
        "MACOSX_DEPLOYMENT_TARGET": "13.0"
      },
      "conditions": [
        ["OS=='mac'", {
          "libraries": ["-ldl"]
        }],
        ["OS!='mac'", {
          "libraries": ["-ldl"]
        }]
      ]
    }
  ]
}
