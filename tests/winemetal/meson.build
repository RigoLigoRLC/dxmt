presentation_feedback_test = executable('presentation-feedback-test',
  'presentation_feedback.c',
  c_args : ['-ObjC', '-fobjc-arc', '-fblocks'],
  include_directories : include_directories('../../src/winemetal/unix'),
  link_with : presentation_feedback,
  link_args : ['-arch', native_compiler_arch],
  dependencies : dependency('appleframeworks', modules: ['Foundation', 'Metal'], native: true),
  native : true,
  install : false,
)

test('presentation-feedback', presentation_feedback_test, timeout: 15)
