package("PLzmaSDK")
    set_sourcedir(path.absolute("macos-launcher/deps/PLzmaSDK"))
    
    on_install("macosx", function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DLIBPLZMA_OPT_SHARED=OFF")
        table.insert(configs, "-DLIBPLZMA_OPT_STATIC=ON")
        table.insert(configs, "-DLIBPLZMA_OPT_TESTS=OFF")
        table.insert(configs, "-DLIBPLZMA_OPT_NO_CRYPTO=OFF")
        table.insert(configs, "-DLIBPLZMA_OPT_NO_PROGRESS=OFF")
        table.insert(configs, "-DLIBPLZMA_OPT_HAVE_STD=ON")
        
        import("package.tools.cmake").install(package, configs)
    end)
    
    on_test(function(package)
        assert(package:has_cxxfuncs("plzma_decoder_create", {includes = "libplzma.hpp", configs = {languages = "c++11"}}))
    end)
package_end()

