if RUBY_PLATFORM =~ /darwin/
  STDERR.puts "\n\n"
  STDERR.puts "***************************************************************************************"
  STDERR.puts "**************************** osx is not supported (yet) =( ****************************"
  STDERR.puts "***************************************************************************************"
  exit(1)
end

if RUBY_VERSION >= "1.9"
  begin
    require 'ruby_core_source'
  rescue LoadError
    retry if require 'rubygems'
    STDERR.puts "\n\n"
    STDERR.puts "***************************************************************************************"
    STDERR.puts "******************** PLEASE RUN gem install ruby_core_source FIRST ********************"
    STDERR.puts "***************************************************************************************"
    exit(1)
  end
end

require 'mkmf'
require 'fileutils'

CWD = File.expand_path(File.dirname(__FILE__))

def sys(cmd)
  puts "  -- #{cmd}"
  unless ret = xsystem(cmd)
    raise "#{cmd} failed, please report to memprof@tmm1.net with pastie.org link to #{CWD}/mkmf.log"
  end
  ret
end

###
# yajl

yajl = File.basename('yajl-1.0.8.tar.gz')
dir = File.basename(yajl, '.tar.gz')

unless File.exists?("#{CWD}/dst/lib/libyajl_ext.a")
  puts "(I'm about to compile yajl.. this will definitely take a while)"

  Dir.chdir('src') do
    FileUtils.rm_rf(dir) if File.exists?(dir)

    sys("tar zxvf #{yajl}")
    Dir.chdir("#{dir}/src") do
      FileUtils.mkdir_p "api/yajl"
      %w[ common parse gen ].each do |f|
        FileUtils.cp "api/yajl_#{f}.h", 'api/yajl/'
      end

      File.open("extconf.rb",'w') do |f|
        f.puts "require 'mkmf'; $INCFLAGS[0,0] = '-I./api/ '; create_makefile 'libyajl'"
      end
      sys("#{Config::CONFIG['bindir']}/#{Config::CONFIG['ruby_install_name']} extconf.rb")

      sys("make")
      sys("ar rv libyajl_ext.a #{Dir['*.o'].join(' ')}")

      FileUtils.mkdir_p "#{CWD}/dst/lib"
      FileUtils.cp 'libyajl_ext.a', "#{CWD}/dst/lib"
      FileUtils.mkdir_p "#{CWD}/dst/include"
      FileUtils.cp_r 'api/yajl', "#{CWD}/dst/include/"
    end
  end
end

$LIBPATH.unshift "#{CWD}/dst/lib"
$INCFLAGS[0,0] = "-I#{CWD}/dst/include "

unless have_library('yajl_ext') and have_header('yajl/yajl_gen.h')
  raise 'Yajl build failed'
end

def add_define(name)
  $defs.push("-D#{name}")
end

if RUBY_PLATFORM =~ /linux/
  ###
  # libelf

  libelf = File.basename('libelf-0.8.13.tar.gz')
  dir = File.basename(libelf, '.tar.gz')

  unless File.exists?("#{CWD}/dst/lib/libelf_ext.a")
    puts "(I'm about to compile libelf.. this will definitely take a while)"

    Dir.chdir('src') do
      FileUtils.rm_rf(dir) if File.exists?(dir)

      sys("tar zxvf #{libelf}")
      Dir.chdir(dir) do
        ENV['CFLAGS'] = '-fPIC'
        sys("./configure --prefix=#{CWD}/dst --disable-nls --disable-shared")
        sys("make")
        sys("make install")
      end
    end

    Dir.chdir('dst/lib') do
      FileUtils.ln_s 'libelf.a', 'libelf_ext.a'
    end
  end

  $LIBPATH.unshift "#{CWD}/dst/lib"
  $INCFLAGS[0,0] = "-I#{CWD}/dst/include "

  unless have_library('elf_ext', 'gelf_getshdr')
    raise 'libelf build failed'
  end

  ###
  # libdwarf

  libdwarf = File.basename('libdwarf-20091118.tar.gz')
  dir = File.basename(libdwarf, '.tar.gz').sub('lib','')

  unless File.exists?("#{CWD}/dst/lib/libdwarf_ext.a")
    puts "(I'm about to compile libdwarf.. this will definitely take a while)"

    Dir.chdir('src') do
      FileUtils.rm_rf(dir) if File.exists?(dir)

      sys("tar zxvf #{libdwarf}")
      Dir.chdir("#{dir}/libdwarf") do
        ENV['CFLAGS'] = "-fPIC -I#{CWD}/dst/include"
        ENV['LDFLAGS'] = "-L#{CWD}/dst/lib"
        sys("./configure")
        sys("make")

        FileUtils.cp 'libdwarf.a', "#{CWD}/dst/lib/libdwarf_ext.a"
        FileUtils.cp 'dwarf.h', "#{CWD}/dst/include/"
        FileUtils.cp 'libdwarf.h', "#{CWD}/dst/include/"
      end
    end
  end

  unless have_library('dwarf_ext')
    raise 'libdwarf build failed'
  end

  is_elf = true
  add_define 'HAVE_ELF'
  add_define 'HAVE_DWARF'
end

if have_header('mach-o/dyld')
  is_macho = true
  add_define 'HAVE_MACH'
end

arch = RUBY_PLATFORM[/(.*)-linux/,1]
if arch == 'universal'
  arch = 'x86_64'
end
add_define "_ARCH_#{arch}_"

if is_elf or is_macho
  if RUBY_VERSION >= "1.9"
    hdrs = proc {
      have_header("vm_core.h") and
      have_header("iseq.h") and
      have_header("insns.inc") and
      have_header("insns_info.inc")
    }
    Ruby_core_source::create_makefile_with_core(hdrs, "memprof")
  else
    create_makefile("memprof")
  end
else
  raise 'unsupported platform'
end
