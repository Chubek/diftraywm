#!/usr/bin/env perl
use strict;
use warnings;

# Usage example:
# cat <<'END' | ./bindings/generate-bindings.pl --yaml --input STDIN
# targets:
#   - Python:
#       - name: "py-%pkgnm"
#       - destination: "$HOME/py-%pkgnm"
#       - exec:
#           - with: "/usr/bin/bash"
#           - script: |
#               echo "building $PKGNAME in $DESTDIR"
#       - XFeats:
#           - "fn-decorators"
#           - "py-properties"
# END

use Getopt::Long qw(GetOptions);
use File::Path qw(make_path);
use File::Basename qw(dirname);
use Cwd qw(abs_path getcwd);
use JSON::PP qw(decode_json);

my $yaml_impl;
BEGIN {
  if (eval { require YAML::PP; 1 }) {
    $yaml_impl = 'YAML::PP';
  } elsif (eval { require YAML::XS; 1 }) {
    $yaml_impl = 'YAML::XS';
  } else {
    die "Missing YAML parser: install YAML::PP or YAML::XS\n";
  }
}

my $format = 'yaml';
my $input = 'STDIN';
my $help = 0;
my $fail_fast = 0;
my $dry_run = 0;

GetOptions(
  'yaml'      => sub { $format = 'yaml' },
  'json'      => sub { $format = 'json' },
  'xml'       => sub { $format = 'xml' },
  'sexp'      => sub { $format = 'sexp' },
  'input=s'   => \$input,
  'fail-fast' => \$fail_fast,
  'dry-run'   => \$dry_run,
  'help'      => \$help,
) or die usage();

if ($help) {
  print usage();
  exit 0;
}

my $script_dir = dirname(abs_path($0));
my $iface = "$script_dir/libshell.i";
my $xfeats_manifest = "$script_dir/XFeats.yaml";

-f $iface or die "Missing interface file: $iface\n";
-f $xfeats_manifest or die "Missing XFeats manifest: $xfeats_manifest\n";

my $swig_bin = find_swig();
defined $swig_bin or die "Missing required binary: swig\n";

my $module_name = parse_module_name($iface);
my $xfeats = load_xfeats($xfeats_manifest);
my $config_text = slurp_input($input);
my $config = parse_config($format, $config_text);

ref($config) eq 'HASH' or die "Config root must be a mapping/object\n";
my $targets = $config->{targets};
ref($targets) eq 'ARRAY' or die "Config key 'targets' must be a list\n";

my @summary;
my $had_failure = 0;

for my $entry (@$targets) {
  my ($language, $items) = normalize_target_entry($entry);
  my $spec = normalize_kv_list($items, "target '$language'");

  my $name = require_scalar($spec, 'name', $language);
  my $dest = require_scalar($spec, 'destination', $language);
  my $exec = normalize_exec($spec->{exec}, $language);
  my $target_xfeats = normalize_xfeats($spec->{XFeats}, $language);

  my $expanded_name = expand_template($name, $module_name);
  my $expanded_dest = expand_shell_vars(expand_template($dest, $module_name));

  my ($swig_flag, $swig_macro) = swig_language_mapping($language);
  validate_target_xfeats($xfeats, $target_xfeats, $language);

  my @feature_defines = map { '-D' . xfeat_macro($_) } @$target_xfeats;
  my $wrapper_file = "$expanded_dest/${module_name}_wrap.cxx";

  my @swig_cmd = (
    $swig_bin,
    $swig_flag,
    '-c++',
    "-D$swig_macro",
    @feature_defines,
    '-outdir', $expanded_dest,
    '-o', $wrapper_file,
    $iface,
  );

  my $status = {
    language    => $language,
    name        => $expanded_name,
    destination => $expanded_dest,
    exit_code   => 0,
    ok          => 1,
    message     => 'ok',
  };

  eval {
    if ($dry_run) {
      print "[dry-run] ";
      print shell_join(@swig_cmd), "\n";
    } else {
      make_path($expanded_dest) unless -d $expanded_dest;
      run_cmd(\@swig_cmd, undef, undef, "swig generation for $language");
    }

    if (defined $exec) {
      my $with = $exec->{with};
      my $script = $exec->{script};
      my @exec_cmd = ($with, '-c', $script);
      my %env = (
        PKGNAME     => $expanded_name,
        DESTDIR     => $expanded_dest,
        MODULE_NAME => $module_name,
        LANGUAGE    => $language,
      );
      if ($dry_run) {
        print "[dry-run] (cwd=", $expanded_dest, ") ";
        print shell_join(map { "$_=$env{$_}" } sort keys %env), " ";
        print shell_join(@exec_cmd), "\n";
      } else {
        run_cmd(\@exec_cmd, $expanded_dest, \%env, "exec script for $language");
      }
    }
  };

  if ($@) {
    chomp(my $error = $@);
    $status->{ok} = 0;
    $status->{message} = $error;
    if ($error =~ /exit code (\d+)/) {
      $status->{exit_code} = $1;
    } else {
      $status->{exit_code} = 1;
    }
    $had_failure = 1;
    warn "$error\n";
  }

  push @summary, $status;
  last if $had_failure && $fail_fast;
}

print_summary(\@summary);
exit($had_failure ? 1 : 0);

sub usage {
  return <<"USAGE";
Usage: $0 [--yaml|--json|--xml|--sexp] --input STDIN|<path> [--fail-fast] [--dry-run] [--help]
USAGE
}

sub slurp_input {
  my ($source) = @_;
  if ($source eq 'STDIN') {
    local $/ = undef;
    my $text = <STDIN>;
    defined $text or die "No data read from STDIN\n";
    return $text;
  }
  -f $source or die "Input file does not exist: $source\n";
  open my $fh, '<', $source or die "Cannot open input file '$source': $!\n";
  local $/ = undef;
  my $text = <$fh>;
  close $fh;
  defined $text or die "No data read from '$source'\n";
  return $text;
}

sub parse_config {
  my ($fmt, $text) = @_;
  if ($fmt eq 'yaml') {
    return load_yaml($text);
  }
  if ($fmt eq 'json') {
    my $cfg = eval { decode_json($text) };
    $@ and die "Invalid JSON config: $@\n";
    return $cfg;
  }
  if ($fmt eq 'xml') {
    return parse_xml_config($text);
  }
  if ($fmt eq 'sexp') {
    return parse_sexp_config($text);
  }
  die "Unsupported format: $fmt\n";
}

sub load_yaml {
  my ($text) = @_;
  if ($yaml_impl eq 'YAML::PP') {
    my $loader = YAML::PP->new();
    my $cfg = eval { $loader->load_string($text) };
    $@ and die "Invalid YAML config: $@\n";
    return $cfg;
  }
  my $cfg = eval { YAML::XS::Load($text) };
  $@ and die "Invalid YAML config: $@\n";
  return $cfg;
}

sub parse_xml_config {
  my ($text) = @_;
  eval { require XML::Simple; 1 } or die "XML parsing requires XML::Simple for --xml\n";
  my $xs = XML::Simple->new(ForceArray => 1, KeyAttr => []);
  my $raw = eval { $xs->XMLin($text) };
  $@ and die "Invalid XML config: $@\n";
  my $targets = $raw->{targets}[0]{target} || [];
  my @norm_targets;
  for my $t (@$targets) {
    my $lang = $t->{language} // die "XML target missing attribute 'language'\n";
    my @items;
    push @items, { name => scalar($t->{name}[0] // '') };
    push @items, { destination => scalar($t->{destination}[0] // '') };
    if (exists $t->{exec}) {
      my $e = $t->{exec}[0];
      my @exec_items;
      push @exec_items, { with => scalar($e->{with}[0] // '') };
      push @exec_items, { script => scalar($e->{script}[0] // '') };
      push @items, { exec => \@exec_items };
    }
    my @xfeats = ();
    if (exists $t->{XFeats} && exists $t->{XFeats}[0]{id}) {
      @xfeats = map { "$_" } @{ $t->{XFeats}[0]{id} };
    }
    push @items, { XFeats => \@xfeats };
    push @norm_targets, { $lang => \@items };
  }
  return { targets => \@norm_targets };
}

sub parse_sexp_config {
  my ($text) = @_;
  my @tokens = tokenize_sexp($text);
  my $node = read_sexp(\@tokens);
  !@tokens or die "Invalid S-expression config: trailing tokens\n";
  ref($node) eq 'ARRAY' or die "Invalid S-expression root\n";
  my %cfg;
  for my $item (@$node) {
    ref($item) eq 'ARRAY' && @$item >= 2 or next;
    my $key = shift @$item;
    if ($key eq 'targets') {
      my @targets;
      for my $target (@$item) {
        ref($target) eq 'ARRAY' && @$target >= 1 or next;
        my $language = shift @$target;
        my @fields;
        for my $f (@$target) {
          ref($f) eq 'ARRAY' && @$f >= 2 or next;
          my $fk = shift @$f;
          if ($fk eq 'XFeats') {
            push @fields, { XFeats => [ map { ref($_) ? () : "$_" } @$f ] };
          } elsif ($fk eq 'exec') {
            my @exec_items;
            for my $e (@$f) {
              ref($e) eq 'ARRAY' && @$e == 2 or next;
              push @exec_items, { $e->[0] => $e->[1] };
            }
            push @fields, { exec => \@exec_items };
          } else {
            push @fields, { $fk => $f->[0] };
          }
        }
        push @targets, { $language => \@fields };
      }
      $cfg{targets} = \@targets;
    }
  }
  return \%cfg;
}

sub tokenize_sexp {
  my ($s) = @_;
  my @t;
  while ($s =~ /\G\s*(?:([()])|"((?:\\.|[^"])*)"|([^\s()]+))/gc) {
    if (defined $1) { push @t, $1; next; }
    if (defined $2) { (my $q = $2) =~ s/\\"/"/g; push @t, $q; next; }
    push @t, $3;
  }
  return @t;
}

sub read_sexp {
  my ($tokens) = @_;
  @$tokens or die "Invalid S-expression config: unexpected EOF\n";
  my $tok = shift @$tokens;
  if ($tok eq '(') {
    my @list;
    while (@$tokens && $tokens->[0] ne ')') {
      push @list, read_sexp($tokens);
    }
    @$tokens or die "Invalid S-expression config: missing ')'\n";
    shift @$tokens;
    return \@list;
  }
  die "Invalid S-expression config: unexpected ')'\n" if $tok eq ')';
  return $tok;
}

sub load_xfeats {
  my ($path) = @_;
  open my $fh, '<', $path or die "Cannot open $path: $!\n";
  local $/ = undef;
  my $yaml = <$fh>;
  close $fh;
  my $doc = load_yaml($yaml);
  ref($doc) eq 'HASH' or die "Invalid XFeats manifest: root must be mapping\n";
  my $features = $doc->{Features};
  ref($features) eq 'ARRAY' or die "Invalid XFeats manifest: 'Features' must be list\n";
  my %index;
  for my $f (@$features) {
    ref($f) eq 'HASH' or next;
    my $id = $f->{id} // die "Invalid XFeats manifest entry missing id\n";
    my $languages = $f->{languages};
    ref($languages) eq 'ARRAY' or die "XFeat '$id' missing languages list\n";
    $index{$id} = {
      languages => { map { $_ => 1 } @$languages },
    };
  }
  return \%index;
}

sub parse_module_name {
  my ($path) = @_;
  open my $fh, '<', $path or die "Cannot open $path: $!\n";
  while (my $line = <$fh>) {
    if ($line =~ /^\s*%module(?:\s*\([^)]+\))?\s+([A-Za-z_]\w*)/) {
      close $fh;
      return $1;
    }
  }
  close $fh;
  die "Could not parse %module from $path\n";
}

sub normalize_target_entry {
  my ($entry) = @_;
  ref($entry) eq 'HASH' or die "Each targets entry must be a mapping\n";
  my @keys = keys %$entry;
  @keys == 1 or die "Each targets entry must have exactly one language key\n";
  my $language = $keys[0];
  my $items = $entry->{$language};
  ref($items) eq 'ARRAY' or die "Target '$language' must map to a list of key/value entries\n";
  return ($language, $items);
}

sub normalize_kv_list {
  my ($items, $ctx) = @_;
  my %out;
  for my $kv (@$items) {
    ref($kv) eq 'HASH' or die "Invalid $ctx entry: expected mapping item\n";
    my @keys = keys %$kv;
    @keys == 1 or die "Invalid $ctx entry: each mapping item must contain exactly one key\n";
    $out{$keys[0]} = $kv->{$keys[0]};
  }
  return \%out;
}

sub normalize_exec {
  my ($exec_raw, $language) = @_;
  return undef if !defined $exec_raw;
  if (ref($exec_raw) eq 'ARRAY') {
    my $exec = normalize_kv_list($exec_raw, "exec for '$language'");
    my $with = require_scalar($exec, 'with', "$language.exec");
    my $script = require_scalar($exec, 'script', "$language.exec");
    return { with => $with, script => $script };
  }
  if (ref($exec_raw) eq 'HASH') {
    my $with = require_scalar($exec_raw, 'with', "$language.exec");
    my $script = require_scalar($exec_raw, 'script', "$language.exec");
    return { with => $with, script => $script };
  }
  die "Invalid exec block for '$language': expected list or mapping\n";
}

sub normalize_xfeats {
  my ($xfeats_raw, $language) = @_;
  return [] if !defined $xfeats_raw;
  ref($xfeats_raw) eq 'ARRAY' or die "Invalid XFeats for '$language': expected list\n";
  for my $id (@$xfeats_raw) {
    !ref($id) or die "Invalid XFeats entry for '$language': ids must be scalars\n";
  }
  return [ map { "$_" } @$xfeats_raw ];
}

sub require_scalar {
  my ($hash, $key, $ctx) = @_;
  exists $hash->{$key} or die "Missing required key '$key' in target '$ctx'\n";
  defined $hash->{$key} && !ref($hash->{$key}) or die "Key '$key' in target '$ctx' must be a scalar\n";
  return "$hash->{$key}";
}

sub validate_target_xfeats {
  my ($xfeats_index, $ids, $language) = @_;
  for my $id (@$ids) {
    exists $xfeats_index->{$id}
      or die "XFeat validation failed: unknown id '$id' for language '$language'\n";
    $xfeats_index->{$id}{languages}{$language}
      or die "XFeat validation failed: id '$id' does not support language '$language'\n";
  }
}

sub expand_template {
  my ($value, $module_name) = @_;
  $value =~ s/%pkgnm/$module_name/g;
  return $value;
}

sub expand_shell_vars {
  my ($value) = @_;
  $value =~ s/\$(\w+)/defined $ENV{$1} ? $ENV{$1} : ''/ge;
  $value =~ s/\$\{(\w+)\}/defined $ENV{$1} ? $ENV{$1} : ''/ge;
  return $value;
}

sub swig_language_mapping {
  my ($language) = @_;
  my %map = (
    'Python'     => ['-python', '-DSWIGPYTHON', 'SWIGPYTHON'],
    'Ruby'       => ['-ruby', '-DSWIGRUBY', 'SWIGRUBY'],
    'C#'         => ['-csharp', '-DSWIGCSHARP', 'SWIGCSHARP'],
    'Lua'        => ['-lua', '-DSWIGLUA', 'SWIGLUA'],
    'Java'       => ['-java', '-DSWIGJAVA', 'SWIGJAVA'],
    'Go'         => ['-go', '-DSWIGGO', 'SWIGGO'],
    'PHP'        => ['-php', '-DSWIGPHP', 'SWIGPHP'],
    'Perl'       => ['-perl5', '-DSWIGPERL', 'SWIGPERL'],
    'R'          => ['-r', '-DSWIGR', 'SWIGR'],
    'D'          => ['-d', '-DSWIGD', 'SWIGD'],
    'JavaScript' => ['-javascript', '-DSWIGJAVASCRIPT', 'SWIGJAVASCRIPT'],
  );
  exists $map{$language} or die "Unsupported target language '$language'\n";
  return ($map{$language}[0], $map{$language}[2]);
}

sub xfeat_macro {
  my ($id) = @_;
  my $name = uc($id);
  $name =~ s/[^A-Z0-9]+/_/g;
  return "XFEAT_$name";
}

sub run_cmd {
  my ($argv, $cwd, $env, $context) = @_;
  my $orig = getcwd();
  if (defined $cwd) {
    make_path($cwd) unless -d $cwd;
    chdir($cwd) or die "Cannot chdir to '$cwd' for $context: $!\n";
  }

  local %ENV = %ENV;
  if (defined $env) {
    for my $k (keys %$env) {
      $ENV{$k} = $env->{$k};
    }
  }

  my $rc = system { $argv->[0] } @$argv;
  if (defined $cwd) {
    chdir($orig) or die "Cannot restore cwd '$orig': $!\n";
  }
  if ($rc == -1) {
    die "Failed to execute $context: $!\n";
  }
  if ($rc & 127) {
    die sprintf("%s terminated by signal %d\n", $context, ($rc & 127));
  }
  my $exit = $rc >> 8;
  $exit == 0 or die "$context failed with exit code $exit\n";
}

sub find_swig {
  for my $path (split /:/, ($ENV{PATH} // '')) {
    my $bin = "$path/swig";
    return $bin if -x $bin;
  }
  return undef;
}

sub shell_join {
  return join ' ', map { shell_quote($_) } @_;
}

sub shell_quote {
  my ($s) = @_;
  return "''" if !defined($s) || $s eq '';
  return $s if $s =~ /\A[-_@%+=:,\.\/A-Za-z0-9]+\z/;
  $s =~ s/'/'"'"'/g;
  return "'$s'";
}

sub print_summary {
  my ($items) = @_;
  print "Summary:\n";
  for my $it (@$items) {
    my $status = $it->{ok} ? 'OK' : 'FAIL';
    print "  - language=$it->{language}; name=$it->{name}; destination=$it->{destination}; exit_code=$it->{exit_code}; status=$status\n";
  }
}
